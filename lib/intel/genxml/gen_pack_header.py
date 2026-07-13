#encoding=utf-8
# SPDX-License-Identifier: MIT

import argparse
import ast
import intel_genxml
import re
import sys
import copy
import textwrap
from util import *

license =  """/*
 * Copyright (C) 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
"""

pack_header = """%(license)s

/* Instructions, enums and structures for %(platform)s.
 *
 * This file has been generated, do not hand edit.
 */

#ifndef %(guard)s
#define %(guard)s

#include <stdio.h>
#include "intel/genxml/igt_genxml_defs.h"

"""

def num_from_str(num_str):
    if num_str.lower().startswith('0x'):
        return int(num_str, base=16)

    assert not num_str.startswith('0'), 'octals numbers not allowed'
    return int(num_str)

def bool_from_str(bool_str):
    options = { "true": True, "false": False }
    return options[bool_str];

class Field(object):
    ufixed_pattern = re.compile(r"u(\d+)\.(\d+)")
    sfixed_pattern = re.compile(r"s(\d+)\.(\d+)")

    def __init__(self, parser, attrs):
        self.parser = parser
        if "name" in attrs:
            self.name = safe_name(attrs["name"])

        dword = int(attrs["dword"])
        end_bit, start_bit = map(int, attrs["bits"].split(":"))

        self.start = dword * 32 + start_bit
        self.end = dword * 32 + end_bit

        self.type = attrs["type"]
        self.nonzero = bool_from_str(attrs.get("nonzero", "false"))
        self.prefix = attrs["prefix"] if "prefix" in attrs else None

        assert self.start <= self.end, \
               'field {} has end ({}) < start ({})'.format(self.name, self.end,
                                                           self.start)
        if self.type == 'bool':
            assert self.end == self.start, \
                   'bool field ({}) is too wide'.format(self.name)

        if "default" in attrs:
            # Base 0 recognizes 0x, 0o, 0b prefixes in addition to decimal ints.
            self.default = int(attrs["default"], base=0)
        else:
            self.default = None

        ufixed_match = Field.ufixed_pattern.match(self.type)
        if ufixed_match:
            self.type = 'ufixed'
            self.fractional_size = int(ufixed_match.group(2))

        sfixed_match = Field.sfixed_pattern.match(self.type)
        if sfixed_match:
            self.type = 'sfixed'
            self.fractional_size = int(sfixed_match.group(2))

    def is_builtin_type(self):
        builtins =  [ 'address', 'bool', 'float', 'ufixed',
                      'offset', 'sfixed', 'offset', 'int', 'uint',
                      'mbo', 'mbz' ]
        return self.type in builtins

    def is_struct_type(self):
        return self.type in self.parser.structs

    def is_enum_type(self):
        return self.type in self.parser.enums

    def emit_template_struct(self, dim):
        if self.type == 'address':
            type = '__gen_address_type'
        elif self.type == 'bool':
            type = 'bool'
        elif self.type == 'float':
            type = 'float'
        elif self.type == 'ufixed':
            type = 'float'
        elif self.type == 'sfixed':
            type = 'float'
        elif self.type == 'uint' and self.end - self.start > 32:
            type = 'uint64_t'
        elif self.type == 'offset':
            type = 'uint64_t'
        elif self.type == 'int':
            type = 'int32_t'
        elif self.type == 'uint':
            type = 'uint32_t'
        elif self.is_struct_type():
            type = 'struct ' + self.parser.gen_prefix_for_type(self.type)
        elif self.is_enum_type():
            type = 'enum ' + self.parser.gen_prefix(safe_name(self.type))
        elif self.type == 'mbo' or self.type == 'mbz':
            return
        else:
            print("#error unhandled type: %s" % self.type)
            return

        print("   %-36s %s%s;" % (type, self.name, dim))

    def emit_value_defines(self):
        for value in self.values:
            defname = self.parser.gen_value_name(
                value.name,
                prefix=self.prefix,
                strip_prefixed_leading_underscore=True)
            print("#ifndef %s" % defname)
            print("#define %-40s %d" % (defname, value.value))
            print("#endif")

class Group(object):
    def __init__(self, parser, parent, start, count, size):
        self.parser = parser
        self.parent = parent
        self.start = start
        self.count = count
        self.size = size
        self.fields = []

    def emit_template_struct(self, dim):
        if self.count == 0:
            print("   /* variable length fields follow */")
        else:
            if self.count > 1:
                dim = "%s[%d]" % (dim, self.count)

            for field in self.fields:
                field.emit_template_struct(dim)

    def emit_value_defines(self):
        for field in self.fields:
            if isinstance(field, Group):
                field.emit_value_defines()
            else:
                field.emit_value_defines()

    class DWord:
        def __init__(self):
            self.size = 32
            self.fields = []
            self.address = None

    def collect_dwords(self, dwords, start, dim):
        for field in self.fields:
            if isinstance(field, Group):
                if field.count == 1:
                    field.collect_dwords(dwords, start + field.start, dim)
                else:
                    for i in range(field.count):
                        field.collect_dwords(dwords,
                                             start + field.start + i * field.size,
                                             "%s[%d]" % (dim, i))
                continue

            index = (start + field.start) // 32
            if not index in dwords:
                dwords[index] = self.DWord()

            clone = copy.copy(field)
            clone.start = clone.start + start
            clone.end = clone.end + start
            clone.dim = dim
            dwords[index].fields.append(clone)

            if field.type == "address":
                # assert dwords[index].address == None
                dwords[index].address = clone

            # Coalesce all the dwords covered by this field. The two cases we
            # handle are where multiple fields are in a 64 bit word (typically
            # and address and a few bits) or where a single struct field
            # completely covers multiple dwords.
            while index < (start + field.end) // 32:
                if index + 1 in dwords and not dwords[index] == dwords[index + 1]:
                    dwords[index].fields.extend(dwords[index + 1].fields)
                dwords[index].size = 64
                dwords[index + 1] = dwords[index]
                index = index + 1

    def collect_dwords_and_length(self, repack=False):
        dwords = {}
        self.collect_dwords(dwords, 0, "")

        # Determine number of dwords in this group. If we have a size, use
        # that, since that'll account for MBZ dwords at the end of a group
        # (like dword 8 on BDW+ 3DSTATE_HS). Otherwise, use the largest dword
        # index we've seen plus one.
        if self.size > 0:
            length = self.size // 32
        elif dwords:
            length = max(dwords.keys()) + 1
        else:
            length = 0

        return (dwords, length)

    def emit_pack_function(self, dwords, length, repack=False):
        for index in range(length):
            # Handle MBZ dwords
            if not index in dwords:
                print("")
                print("   dw[%d] = 0;" % index)
                continue

            # For 64 bit dwords, we aliased the two dword entries in the dword
            # dict it occupies. Now that we're emitting the pack function,
            # skip the duplicate entries.
            dw = dwords[index]
            if index > 0 and index - 1 in dwords and dw == dwords[index - 1]:
                continue

            # Special case: only one field and it's a struct at the beginning
            # of the dword. In this case we pack directly into the
            # destination. This is the only way we handle embedded structs
            # larger than 32 bits.
            if len(dw.fields) == 1:
                field = dw.fields[0]
                name = field.name + field.dim
                if field.is_struct_type() and field.start % 32 == 0:
                    print("")
                    if repack:
                        print("   %s_repack(data, &dw[%d], &origin[%d], &values->%s);" %
                              (self.parser.gen_prefix_for_type(field.type), index, index, name))
                    else:
                        print("   %s_pack(data, &dw[%d], &values->%s);" %
                              (self.parser.gen_prefix_for_type(field.type), index, name))
                    continue

            # Open a block scope for C90 compliance (declarations must
            # precede code within each block).
            print("")
            print("   {")

            # Pack any fields of struct type first so we have integer values
            # to the dword for those fields.
            # Emit all declarations first, then all pack calls (C90).
            field_index = 0
            struct_fields = []
            for field in dw.fields:
                if isinstance(field, Field) and field.is_struct_type():
                    name = field.name + field.dim
                    struct_fields.append((field, name, field_index))
                    field_index = field_index + 1

            if struct_fields:
                for field, name, fi in struct_fields:
                    print("   uint32_t v%d_%d;" % (index, fi))
                for field, name, fi in struct_fields:
                    if repack:
                        print("   %s_repack(data, &v%d_%d, &origin[%d], &values->%s);" %
                              (self.parser.gen_prefix_for_type(field.type), index, fi, index, name))
                    else:
                        print("   %s_pack(data, &v%d_%d, &values->%s);" %
                              (self.parser.gen_prefix_for_type(field.type), index, fi, name))

            dword_start = index * 32
            if dw.address == None:
                address_count = 0
            else:
                address_count = 1

            # Assert in dont_use values
            for field in dw.fields:
                for value in field.values:
                    if value.dont_use:
                        print("   assert(values->%s != %s);" %
                              (field.name,
                               self.parser.gen_value_name(value.name,
                                                          prefix=field.prefix)))

            if dw.size == 32 and dw.address == None:
                v = None
                print("   dw[%d] =" % index)
            elif len(dw.fields) > address_count or repack:
                v = "v%d" % index
                print("   const uint%d_t %s =" % (dw.size, v))
            else:
                v = "0"

            field_index = 0
            non_address_fields = []

            if repack:
                non_address_fields.append("origin[%d]" % index)
                if dw.size > 32:
                    non_address_fields.append("((uint64_t)origin[%d] << 32)" % (index + 1))

            for field in dw.fields:
                if field.type != "mbo" and field.type != "mbz" and field.type != "repack":
                    name = field.name + field.dim

                nz = "_nonzero" if field.nonzero else ""

                if field.type == "repack":
                    non_address_fields.append("origin[%d]" % index)
                elif field.type == "mbo":
                    non_address_fields.append("util_bitpack_ones(%d, %d)" % \
                        (field.start - dword_start, field.end - dword_start))
                elif field.type == "mbz":
                    assert not field.nonzero
                elif field.type == "address":
                    pass
                elif field.type == "uint":
                    non_address_fields.append("util_bitpack_uint%s(values->%s, %d, %d)" % \
                        (nz, name, field.start - dword_start, field.end - dword_start))
                elif field.is_enum_type():
                    non_address_fields.append("util_bitpack_uint%s(values->%s, %d, %d)" % \
                        (nz, name, field.start - dword_start, field.end - dword_start))
                elif field.type == "int":
                    non_address_fields.append("util_bitpack_sint%s(values->%s, %d, %d)" % \
                        (nz, name, field.start - dword_start, field.end - dword_start))
                elif field.type == "bool":
                    non_address_fields.append("util_bitpack_uint%s(values->%s, %d, %d)" % \
                        (nz, name, field.start - dword_start, field.end - dword_start))
                elif field.type == "float":
                    non_address_fields.append("util_bitpack_float%s(values->%s)" % (nz, name))
                elif field.type == "offset":
                    non_address_fields.append("__gen_offset%s(values->%s, %d, %d)" % \
                        (nz, name, field.start - dword_start, field.end - dword_start))
                elif field.type == 'ufixed':
                    non_address_fields.append("util_bitpack_ufixed%s(values->%s, %d, %d, %d)" % \
                        (nz, name, field.start - dword_start, field.end - dword_start, field.fractional_size))
                elif field.type == 'sfixed':
                    non_address_fields.append("util_bitpack_sfixed%s(values->%s, %d, %d, %d)" % \
                        (nz, name, field.start - dword_start, field.end - dword_start, field.fractional_size))
                elif field.is_struct_type():
                    non_address_fields.append("util_bitpack_uint(v%d_%d, %d, %d)" % \
                        (index, field_index, field.start - dword_start, field.end - dword_start))
                    field_index = field_index + 1
                else:
                    non_address_fields.append("/* unhandled field %s, type %s */\n" % \
                                              (name, field.type))

            if non_address_fields:
                print(" |\n".join("      " + f for f in non_address_fields) + ";")

            if dw.size == 32:
                if dw.address:
                    print("   dw[%d] = __gen_address(data, &dw[%d], values->%s, %s, %d, %d);" %
                          (index, index, dw.address.name + field.dim, v,
                           dw.address.start - dword_start, dw.address.end - dword_start))
                print("   }")
                continue

            if dw.address:
                v_address = "v%d_address" % index
                print("   const uint64_t %s =\n      __gen_address(data, &dw[%d], values->%s, %s, %d, %d);" %
                      (v_address, index, dw.address.name + field.dim, v,
                       dw.address.start - dword_start, dw.address.end - dword_start))
                if len(dw.fields) > address_count:
                    print("   dw[%d] = %s;" % (index, v_address))
                    print("   dw[%d] = (%s >> 32) | (%s >> 32);" % (index + 1, v_address, v))
                    print("   }")
                    continue
                else:
                    v = v_address
            print("   dw[%d] = %s;" % (index, v))
            print("   dw[%d] = %s >> 32;" % (index + 1, v))
            print("   }")

class Value(object):
    def __init__(self, attrs):
        self.name = safe_name(attrs["name"])
        self.value = ast.literal_eval(attrs["value"])
        self.dont_use = int(attrs["dont_use"]) != 0 if "dont_use" in attrs else False

class Parser(object):
    def __init__(self, repack):
        self.instruction = None
        self.structs = {}
        # Set of enum names we've seen.
        self.enums = set()
        self.registers = {}
        self.repack = repack
        # Maps struct/register type names to the origin gen label for types
        # that were omitted (no definition emitted for this gen).
        self.skipped_type_origins = {}

    def gen_prefix(self, name):
        if name[0] == "_":
            return 'GFX%s%s' % (self.gen, name)
        return 'GFX%s_%s' % (self.gen, name)

    def gen_prefix_for_type(self, type_name):
        """Like gen_prefix but resolves skipped (omitted) types to their
        origin gen so that struct/pack references still compile."""
        origin = self.skipped_type_origins.get(type_name)
        if origin:
            safe = safe_name(type_name)
            if safe[0] == '_':
                return 'GFX%s%s' % (origin, safe)
            return 'GFX%s_%s' % (origin, safe)
        return self.gen_prefix(safe_name(type_name))

    def gen_value_name(self, value_name, prefix=None,
                       strip_prefixed_leading_underscore=False):
        name = value_name
        if prefix:
            if strip_prefixed_leading_underscore and name[0] == '_':
                name = name[1:]
            name = prefix + "_" + name

        return self.gen_prefix(name.upper())

    def gen_guard(self):
        return self.gen_prefix("{0}_PACK_H".format(self.platform))

    def _should_skip_item(self, item):
        """Check if this item is bit-identical to any previous gen
        and should be skipped (applies to instructions, structs, and
        registers)."""
        if not self.baseline_fingerprints:
            return False
        item_name = item.attrib['name']
        baseline_fp = self.baseline_fingerprints.get(item_name)
        if baseline_fp is None:
            return False  # New item, must emit
        return _item_fingerprint(item) == baseline_fp

    def process_item(self, item):
        name = item.tag
        assert name != "genxml"
        attrs = item.attrib

        # Skip items that are bit-identical to any previous gen.
        # Emit a comment pointing to the oldest gen that defined this
        # layout so that a developer can grep and find it in one hop.
        if name in ("instruction", "struct", "register") and self._should_skip_item(item):
            safe = safe_name(attrs["name"])
            origin = self.origin_gens.get(attrs["name"], self.baseline_gen)
            if safe[0] == "_":
                origin_name = 'GFX%s%s' % (origin, safe)
            else:
                origin_name = 'GFX%s_%s' % (origin, safe)
            print("/* %s omitted: identical to %s */" %
                  (self.gen_prefix(safe), origin_name))
            print('')
            # Register skipped structs/registers so later items can
            # still reference them as field types, resolved to origin gen.
            if name == "struct":
                self.structs[attrs["name"]] = 1
                self.skipped_type_origins[attrs["name"]] = origin
            elif name == "register":
                self.registers[attrs["name"]] = 1
                self.skipped_type_origins[attrs["name"]] = origin
            return

        if name in ("instruction", "struct", "register"):
            if name == "instruction":
                self.instruction = safe_name(attrs["name"])
                self.length_bias = int(attrs["bias"])
            elif name == "struct":
                self.struct = safe_name(attrs["name"])
                self.structs[attrs["name"]] = 1
            elif name == "register":
                self.register = safe_name(attrs["name"])
                self.reg_num = num_from_str(attrs["num"])
                self.registers[attrs["name"]] = 1
            if "length" in attrs:
                self.length = int(attrs["length"])
                size = self.length * 32
            else:
                self.length = None
                size = 0
            self.group = Group(self, None, 0, 1, size)

        elif name == "group":
            dword = int(attrs["dword"])
            offset_bits = int(attrs.get("offset_bits", 0))
            start = dword * 32 + offset_bits


            group = Group(self, self.group,
                          start, int(attrs["count"]), int(attrs["size"]))
            self.group.fields.append(group)
            self.group = group
        elif name == "field":
            self.group.fields.append(Field(self, attrs))
            self.values = []
        elif name == "enum":
            self.values = []
            self.enum = safe_name(attrs["name"])
            self.enums.add(attrs["name"])
            if "prefix" in attrs:
                self.prefix = safe_name(attrs["prefix"])
            else:
                self.prefix = None
        elif name == "value":
            self.values.append(Value(attrs))
        elif name in ("import", "exclude"):
            pass
        else:
            assert False

        for child_item in item:
            self.process_item(child_item)

        if name  == "instruction":
            self.emit_instruction()
            self.instruction = None
            self.group = None
        elif name == "struct":
            self.emit_struct()
            self.struct = None
            self.group = None
        elif name == "register":
            self.emit_register()
            self.register = None
            self.reg_num = None
            self.group = None
        elif name == "group":
            self.group = self.group.parent
        elif name  == "field":
            self.group.fields[-1].values = self.values
        elif name  == "enum":
            self.emit_enum()
            self.enum = None
        elif name in ("import", "exclude", "value"):
            pass
        else:
            assert False

    def emit_template_struct(self, name, group):
        print("struct %s {" % self.gen_prefix(name))
        group.emit_template_struct("")
        print("};\n")


    def emit_pack_function(self, name, group, repack=False):
        name = self.gen_prefix(name)
        if repack:
            print(textwrap.dedent("""\
            static inline __attribute__((always_inline)) void
            %s_repack(__attribute__((unused)) __gen_user_data *data,
                    %s__attribute__((unused)) void * restrict dst,
                    %s__attribute__((unused)) const uint32_t * origin,
                    %s__attribute__((unused)) const struct %s * restrict values)
            {""") % (name, ' ' * len(name), ' ' * len(name), ' ' * len(name), name))
        else:
            print(textwrap.dedent("""\
            static inline __attribute__((always_inline)) void
            %s_pack(__attribute__((unused)) __gen_user_data *data,
                  %s__attribute__((unused)) void * restrict dst,
                  %s__attribute__((unused)) const struct %s * restrict values)
            {""") % (name, ' ' * len(name), ' ' * len(name), name))

        (dwords, length) = group.collect_dwords_and_length(repack)
        if length:
            # Cast dst to make header C++ friendly
            type_name = "uint32_t * restrict"
            print("   %s dw = (%s) dst;" % (type_name, type_name))

            group.emit_pack_function(dwords, length, repack)

        print("}\n")

    def emit_instruction(self):
        name = self.instruction

        if not self.length is None:
            print('#define %-33s %6d' %
                  (self.gen_prefix(name + "_length"), self.length))
        print('#define %-33s %6d' %
              (self.gen_prefix(name + "_length_bias"), self.length_bias))

        default_fields = []
        for field in self.group.fields:
            if not isinstance(field, Field):
                continue
            if field.default is None:
                continue

            if field.is_builtin_type():
                default_fields.append("   .%-35s = %6d" % (field.name, field.default))
            else:
                # Default values should not apply to structures
                assert field.is_enum_type()
                default_fields.append("   .%-35s = (enum %s) %6d" % (field.name, self.gen_prefix(safe_name(field.type)), field.default))

        if default_fields:
            print('#define %-40s\\' % (self.gen_prefix(name + '_header')))
            print(",  \\\n".join(default_fields))
            print('')

        self.emit_template_struct(self.instruction, self.group)
        self.emit_pack_function(self.instruction, self.group)
        if self.repack:
            self.emit_pack_function(self.instruction, self.group, repack=True)
        self.group.emit_value_defines()

    def emit_register(self):
        name = self.register
        if not self.reg_num is None:
            print('#define %-33s 0x%04x' %
                  (self.gen_prefix(name + "_num"), self.reg_num))

        if not self.length is None:
            print('#define %-33s %6d' %
                  (self.gen_prefix(name + "_length"), self.length))

        self.emit_template_struct(self.register, self.group)
        self.emit_pack_function(self.register, self.group)
        self.group.emit_value_defines()

    def emit_struct(self):
        name = self.struct
        if not self.length is None:
            print('#define %-33s %6d' %
                  (self.gen_prefix(name + "_length"), self.length))

        self.emit_template_struct(self.struct, self.group)
        self.emit_pack_function(self.struct, self.group)
        if self.repack:
            self.emit_pack_function(self.struct, self.group, repack=True)
        self.group.emit_value_defines()

    def emit_enum(self):
        enum_name = self.gen_prefix(self.enum)
        print('enum %s {' % enum_name)
        for value in self.values:
            name = self.gen_value_name(value.name, prefix=self.prefix)
            print('   %-36s = %6d,' % (name.upper(), value.value))
        print('};')
        print('')

    def emit_genxml(self, genxml):
        root = genxml.et.getroot()
        self.platform = root.attrib["name"]
        self.gen = root.attrib["gen"].replace('.', '')
        print(pack_header % {'license': license, 'platform': self.platform, 'guard': self.gen_guard()})
        for item in root:
            self.process_item(item)
        print('#endif /* %s */' % self.gen_guard())

def _element_fingerprint(elem, skip_attrs=frozenset()):
    """Canonical string representation of an XML element for structural
    comparison.  Captures tag, attributes (sorted, minus skip_attrs),
    and all children recursively."""
    parts = [elem.tag]
    for k, v in sorted(elem.attrib.items()):
        if k not in skip_attrs:
            parts.append('%s=%s' % (k, v))
    for child in elem:
        parts.append(_element_fingerprint(child))
    return '(%s)' % '|'.join(parts)

def _item_fingerprint(item):
    """Structural fingerprint for an XML element (instruction, struct,
    or register).  Skips identity attributes ('name', 'engine', 'num')
    so that two items are considered identical when their bit-layout
    matches, regardless of naming or register offset."""
    return _element_fingerprint(item, skip_attrs={'name', 'engine', 'num'})

def _build_baseline(xml_source, engines):
    """Load a single baseline gen's XML file and build a dict of
    item name -> fingerprint.  Kept for use by gen_decode_header."""
    genxml = intel_genxml.GenXml(xml_source)
    genxml.merge_imported()
    genxml.filter_engines(engines)
    root = genxml.et.getroot()
    baseline = {}
    baseline_gen = root.attrib["gen"].replace('.', '')
    for item in root:
        if item.tag in ('instruction', 'struct', 'register'):
            item_name = item.attrib['name']
            baseline[item_name] = _item_fingerprint(item)
    return baseline, baseline_gen

def _build_baselines(xml_sources, engines):
    """Load a sequence of baseline gen XML files (oldest first) and build:
     - fingerprints: item_name -> fingerprint (from most recent occurrence)
     - baseline_gen: gen label of the most recent baseline
     - origin_gens: item_name -> gen label of the oldest gen with the
       current fingerprint for that item
    An item's origin resets when its fingerprint changes."""
    fingerprints = {}
    origin_gens = {}
    baseline_gen = None
    for xml_source in xml_sources:
        genxml = intel_genxml.GenXml(xml_source)
        genxml.merge_imported()
        genxml.filter_engines(engines)
        root = genxml.et.getroot()
        gen_label = root.attrib["gen"].replace('.', '')
        baseline_gen = gen_label
        for item in root:
            if item.tag in ('instruction', 'struct', 'register'):
                item_name = item.attrib['name']
                fp = _item_fingerprint(item)
                if item_name not in fingerprints or fingerprints[item_name] != fp:
                    # New item or fingerprint changed: this gen is the origin
                    fingerprints[item_name] = fp
                    origin_gens[item_name] = gen_label
                    # else: same fingerprint, keep existing (older) origin
    return fingerprints, baseline_gen, origin_gens

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('xml_source', metavar='XML_SOURCE',
                   help="Input xml file")
    p.add_argument('--engines', nargs='?', type=str, default='render',
                   help="Comma-separated list of engines whose instructions should be parsed (default: %(default)s)")
    p.add_argument('--include-symbols', nargs='?', type=str, action='store',
                   help="List of instruction/structures to generate")
    p.add_argument('--repack', action='store_true', help="Emit repacking code")
    p.add_argument('--baseline', type=str, action='append', default=[],
                   help="Previous gen XML files (oldest first); skip items identical to any baseline")

    pargs = p.parse_args()

    if pargs.engines is None:
        print("No engines specified")
        sys.exit(1)

    return pargs

def main():
    pargs = parse_args()

    engines = set(pargs.engines.split(','))
    valid_engines = [ 'render', 'blitter', 'video', 'compute' ]
    if engines - set(valid_engines):
        print("Invalid engine specified, valid engines are:\n")
        for e in valid_engines:
            print("\t%s" % e)
        sys.exit(1)

    baseline_fingerprints = {}
    baseline_gen = None
    origin_gens = {}
    if pargs.baseline:
        baseline_fingerprints, baseline_gen, origin_gens = _build_baselines(
            pargs.baseline, engines)

    genxml = intel_genxml.GenXml(pargs.xml_source)

    genxml.merge_imported()
    genxml.filter_engines(engines)
    if pargs.include_symbols:
        genxml.filter_symbols(pargs.include_symbols.split(','))
    p = Parser(pargs.repack)
    p.baseline_fingerprints = baseline_fingerprints
    p.baseline_gen = baseline_gen
    p.origin_gens = origin_gens
    p.emit_genxml(genxml)

if __name__ == '__main__':
    main()
