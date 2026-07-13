#encoding=utf-8
# SPDX-License-Identifier: MIT
#
# Copyright (C) 2026 Intel Corporation

"""Generate batch-buffer decode/annotate headers from genxml.

For each instruction, emits a _decode() function that prints annotated
dwords with field names and values. Enum fields show symbolic names
alongside numeric values.

Usage:
    python gen_decode_header.py [--baseline BASELINE.xml] [--engines ...] INPUT.xml
"""

import argparse
import intel_genxml
import sys
from util import safe_name

license = """/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Intel Corporation
 *
 * Batch buffer decode functions for %s.
 *
 * This file has been generated, do not hand edit.
 */
"""


def field_mask(bits):
    """Return (start_bit, end_bit, mask) for a field."""
    end_bit, start_bit = map(int, bits.split(':'))
    width = end_bit - start_bit + 1
    mask = (1 << width) - 1
    return start_bit, end_bit, mask


def compute_opcode_key(instruction):
    """Compute opcode key and mask from default field values in DW0.

    Returns (key, mask) where key has the default values shifted into
    position and mask has 1s for every bit covered by a field with a
    default. For variable-length instructions, DWord Length is excluded so
    one opcode entry can match different payload sizes. For fixed-length
    instructions, DWord Length stays part of the key so newer packet layouts
    do not overmatch older variants that reuse the same opcode.
    Using the full mask for dispatch avoids collisions between
    instructions that share the same top 16 bits but differ in lower
    opcode fields (e.g. 3DPRIMITIVE vs 3DPRIMITIVE_EXTENDED).
    """
    key = 0
    mask = 0
    variable_length = get_length(instruction) is None
    for field in instruction:
        if field.tag != 'field':
            continue
        if int(field.attrib.get('dword', '-1')) != 0:
            continue
        default = field.attrib.get('default')
        if default is None:
            continue
        if variable_length and field.attrib.get('name') == 'DWord Length':
            continue
        start_bit, end_bit, field_mask_val = field_mask(field.attrib['bits'])
        key |= (int(default, 0) & field_mask_val) << start_bit
        mask |= field_mask_val << start_bit
    return key, mask


def get_length_bias(instruction):
    return int(instruction.attrib.get('bias', '2'))


def get_length(instruction):
    return instruction.attrib.get('length')


def collect_enum_values(field_elem, enum_defs):
    """Collect value->name mapping from inline <value> children or
    from a referenced standalone <enum>."""
    values = {}

    # Inline values (children of the <field>)
    for v in field_elem:
        if v.tag == 'value':
            vname = safe_name(v.attrib['name'])
            vval = int(v.attrib['value'], 0)
            values[vval] = vname

    # If the field type references a standalone enum and we didn't
    # find inline values, look it up
    if not values:
        ftype = field_elem.attrib.get('type', '')
        if ftype in enum_defs:
            for v in enum_defs[ftype]:
                if v.tag == 'value':
                    vname = safe_name(v.attrib['name'])
                    vval = int(v.attrib['value'], 0)
                    values[vval] = vname

    return values


class DecodeGenerator:
    def __init__(self, gen, platform, struct_defs, enum_defs):
        self.gen = gen
        self.platform = platform
        self.struct_defs = struct_defs  # name -> XML element
        self.enum_defs = enum_defs     # name -> XML element
        self.instructions = []

    def gen_prefix(self, name):
        if name[0] == '_':
            return 'GFX%s%s' % (self.gen, name)
        return 'GFX%s_%s' % (self.gen, name)

    def is_enum_type(self, ftype):
        """Check if a field type is a standalone enum or has inline values."""
        return ftype in self.enum_defs

    def is_struct_type(self, ftype):
        return ftype in self.struct_defs

    def is_builtin_type(self, ftype):
        builtins = ('address', 'bool', 'float', 'uint', 'int',
                    'offset', 'mbo', 'mbz')
        if ftype in builtins:
            return True
        # ufixed/sfixed patterns like u4.8, s3.13
        if len(ftype) > 1 and ftype[0] in ('u', 's') and '.' in ftype:
            return True
        return False

    def collect_fields(self, instruction):
        """Collect fields grouped by dword index."""
        by_dw = {}
        variable_groups = []
        self._collect_fields_recursive(instruction, by_dw, variable_groups,
                                       prefix="", dw_offset=0)
        return by_dw, variable_groups

    def _collect_fields_recursive(self, parent, by_dw, variable_groups,
                                  prefix, dw_offset):
        for child in parent:
            if child.tag == 'field':
                dword = int(child.attrib['dword']) + dw_offset
                name = safe_name(child.attrib['name'])
                start_bit, end_bit, mask = field_mask(child.attrib['bits'])
                ftype = child.attrib.get('type', 'uint')

                if ftype in ('mbo', 'mbz'):
                    continue

                # Expand struct-type fields
                if not self.is_builtin_type(ftype) and \
                   not self.is_enum_type(ftype) and \
                   self.is_struct_type(ftype):
                    struct_xml = self.struct_defs[ftype]
                    self._collect_fields_recursive(
                        struct_xml, by_dw, variable_groups,
                        prefix + name + ".",
                        dw_offset + dword)
                    continue

                values = collect_enum_values(child, self.enum_defs)

                if dword not in by_dw:
                    by_dw[dword] = []
                by_dw[dword].append((prefix + name, start_bit, end_bit, ftype, values))

            elif child.tag == 'group':
                group_dw = int(child.attrib.get('dword', '0'))
                count = int(child.attrib.get('count', '1'))
                size_bits = int(child.attrib.get('size', '32'))
                size_dw = size_bits // 32

                if count == 0:
                    group_fields = {}
                    nested_variable_groups = []

                    self._collect_fields_recursive(
                        child, group_fields, nested_variable_groups,
                        prefix="", dw_offset=0)

                    variable_groups.append(
                        (prefix, dw_offset + group_dw, size_dw, group_fields))

                    for nested_prefix, nested_dw, nested_size_dw, nested_fields in nested_variable_groups:
                        variable_groups.append(
                            (nested_prefix,
                             dw_offset + group_dw + nested_dw,
                             nested_size_dw,
                             nested_fields))
                    continue

                for i in range(count):
                    idx_prefix = prefix
                    if count > 1:
                        idx_prefix = "%s[%d]." % (prefix.rstrip('.'), i)
                    self._collect_fields_recursive(
                        child, by_dw, variable_groups, idx_prefix,
                        dw_offset + group_dw + i * size_dw)

    def add_instruction(self, instruction):
        name = safe_name(instruction.attrib['name'])
        prefixed = self.gen_prefix(name)
        opcode_key, opcode_mask = compute_opcode_key(instruction)
        length = get_length(instruction)
        bias = get_length_bias(instruction)
        fields_by_dw, variable_groups = self.collect_fields(instruction)
        self.instructions.append((prefixed, opcode_key, opcode_mask, length,
                                  bias, fields_by_dw, variable_groups))

    def emit_header(self):
        guard = 'GFX%s_%s_DECODE_H' % (self.gen, self.platform.upper())
        print(license % self.platform)
        print('#ifndef %s' % guard)
        print('#define %s' % guard)
        print('')
        print('#include <stdio.h>')
        print('#include <stdint.h>')
        print('')

    def emit_footer(self):
        guard = 'GFX%s_%s_DECODE_H' % (self.gen, self.platform.upper())
        print('#endif /* %s */' % guard)

    def emit_decode_function(self, prefixed, opcode_key, opcode_mask, length, bias,
                             fields_by_dw, variable_groups):
        print('static inline unsigned')
        print('%s_decode(FILE *fp, uint32_t base, const uint32_t *dw, unsigned remaining)' % prefixed)
        print('{')

        if length is not None:
            print('   unsigned len = %s;' % length)
        else:
            print('   unsigned len = (dw[0] & 0xff) + %d;' % bias)

        print('   if (len > remaining)')
        print('      len = remaining;')
        print('   fprintf(fp, "[0x%%04x] 0x%%08x  %s (len=%%u)\\n", base, dw[0], len);' % prefixed)

        if not fields_by_dw and not variable_groups:
            print('   for (unsigned i = 1; i < len; i++)')
            print('      fprintf(fp, "[0x%04x] 0x%08x\\n", base + i * 4, dw[i]);')
            print('   return len;')
            print('}')
            print('')
            return

        max_dw = max(fields_by_dw.keys()) if fields_by_dw else 0
        variable_groups = sorted(variable_groups, key=lambda g: g[1])

        # Decode DW0 fields (subopcode, length, flags) if present
        if 0 in fields_by_dw:
            # Filter out identity fields already shown in the header line
            dw0_fields = [(n, s, e, t, v) for n, s, e, t, v in fields_by_dw[0]
                          if n not in ('DWordLength', 'CommandType',
                                       'CommandSubType', '_3DCommandOpcode',
                                       '_3DCommandSubOpcode', 'MICommandOpcode',
                                       'CommandOpcode', 'InstructionType')]
            if dw0_fields:
                self._emit_dword_decode(0, dw0_fields)

        fixed_limit = variable_groups[0][1] if variable_groups else max_dw + 1

        for dw_idx in range(1, fixed_limit):
            print('   if (%d < len) {' % dw_idx)
            if dw_idx not in fields_by_dw:
                print('   fprintf(fp, "[0x%%04x] 0x%%08x\\n", base + %d, dw[%d]);' %
                      (dw_idx * 4, dw_idx))
            else:
                fields = fields_by_dw[dw_idx]
                self._emit_dword_decode(dw_idx, fields)
            print('   }')

        if variable_groups:
            for group_prefix, start_dw, size_dw, group_fields_by_dw in variable_groups:
                self._emit_runtime_group_decode(group_prefix, start_dw, size_dw,
                                                group_fields_by_dw)
            print('   return len;')
            print('}')
            print('')
            return

        if length is not None:
            total = int(length)
        else:
            total = None

        if total is not None and total > max_dw + 1:
            print('   for (unsigned i = %d; i < %d; i++)' % (max_dw + 1, total))
            print('      fprintf(fp, "[0x%04x] 0x%08x\\n", base + i * 4, dw[i]);')
        elif total is None:
            print('   for (unsigned i = %d; i < len; i++)' % (max_dw + 1,))
            print('      fprintf(fp, "[0x%04x] 0x%08x\\n", base + i * 4, dw[i]);')

        print('   return len;')
        print('}')
        print('')

    def _emit_enum_decode(self, var_expr, values, sep, name, indent='   '):
        """Emit a switch that prints 'NAME (num)' for known values,
        or just 'num' for unknown ones. Always shows the raw number."""
        print('%s{ uint32_t _v = %s;' % (indent, var_expr))
        print('%s  fprintf(fp, "%s.%s = ");' % (indent, sep, name))
        print('%s  switch (_v) {' % indent)
        for val, vname in sorted(values.items()):
            print('%s  case %d: fprintf(fp, "%s (%%u)", _v); break;' % (indent, val, vname))
        print('%s  default: fprintf(fp, "%%u", _v); break;' % indent)
        print('%s  }' % indent)
        print('%s}' % indent)

    def _emit_dword_decode(self, dw_idx, fields):
        """Emit decode for a single dword's fields."""
        fields = sorted(fields, key=lambda f: f[1])

        interesting = [(name, start, end, ftype, values)
                       for name, start, end, ftype, values in fields
                       if ftype not in ('mbo', 'mbz')]

        if not interesting:
            print('   fprintf(fp, "[0x%%04x] 0x%%08x\\n", base + %d, dw[%d]);' %
                  (dw_idx * 4, dw_idx))
            return

        print('   fprintf(fp, "[0x%%04x] 0x%%08x   ", base + %d, dw[%d]);' %
              (dw_idx * 4, dw_idx))

        for i, (name, start, end, ftype, values) in enumerate(interesting):
            width = end - start + 1
            mask = (1 << width) - 1
            sep = ', ' if i > 0 else ''
            extract = '(dw[%d] >> %d) & 0x%x' % (dw_idx, start, mask)

            if ftype == 'bool':
                print('   fprintf(fp, "%s.%s = %%s", %s ? "true" : "false");' %
                      (sep, name, extract))
            elif ftype == 'address':
                # Mask off the low reserved bits using the actual field
                # start position from the XML, not a hardcoded 12.
                if start > 0:
                    addr_mask = (1 << start) - 1
                    print('   fprintf(fp, "%s.%s = 0x%%lx", '
                          '(unsigned long)(((uint64_t)dw[%d] | ((uint64_t)dw[%d] << 32)) & ~0x%xUL));' %
                          (sep, name, dw_idx, dw_idx + 1, addr_mask))
                else:
                    print('   fprintf(fp, "%s.%s = 0x%%lx", '
                          '(unsigned long)((uint64_t)dw[%d] | ((uint64_t)dw[%d] << 32)));' %
                          (sep, name, dw_idx, dw_idx + 1))
            elif ftype == 'float':
                print('   { union { uint32_t u; float f; } _fv = { .u = dw[%d] };' % dw_idx)
                print('     fprintf(fp, "%s.%s = %%f", _fv.f); }' % (sep, name))
            elif values:
                # Enum or field with named values - show symbolic name + raw number
                self._emit_enum_decode(extract, values, sep, name)
            elif width > 32:
                print('   fprintf(fp, "%s.%s = 0x%%lx", '
                      '(unsigned long)((uint64_t)(dw[%d] >> %d) | ((uint64_t)dw[%d] << %d)));' %
                      (sep, name, dw_idx, start, dw_idx + 1, 32 - start))
            else:
                print('   fprintf(fp, "%s.%s = %%u", %s);' %
                      (sep, name, extract))

        print('   fprintf(fp, "\\n");')

    def _emit_runtime_group_decode(self, group_prefix, start_dw, size_dw, fields_by_dw):
        """Emit decode for a runtime-sized trailing group."""
        print('   if (%d < len) {' % start_dw)
        print('      unsigned group_count = (len - %d) / %d;' % (start_dw, size_dw))
        print('      unsigned group_tail = %d + group_count * %d;' % (start_dw, size_dw))
        print('      for (unsigned group_idx = 0; group_idx < group_count; group_idx++) {')
        print('         unsigned elem_dw = %d + group_idx * %d;' % (start_dw, size_dw))

        for rel_dw in range(size_dw):
            fields = fields_by_dw.get(rel_dw, [])
            interesting = [(name, start, end, ftype, values)
                           for name, start, end, ftype, values in sorted(fields, key=lambda f: f[1])
                           if ftype not in ('mbo', 'mbz')]

            if not interesting:
                print('         fprintf(fp, "[0x%%04x] 0x%%08x\\n", '
                      'base + (elem_dw + %d) * 4, dw[elem_dw + %d]);' % (rel_dw, rel_dw))
                continue

            print('         fprintf(fp, "[0x%%04x] 0x%%08x   ", '
                  'base + (elem_dw + %d) * 4, dw[elem_dw + %d]);' % (rel_dw, rel_dw))

            if group_prefix:
                print('         fprintf(fp, "' + group_prefix.rstrip('.') + '[%u] ", group_idx);')
            else:
                print('         fprintf(fp, "[%u] ", group_idx);')

            for i, (name, start, end, ftype, values) in enumerate(interesting):
                width = end - start + 1
                mask = (1 << width) - 1
                sep = ', ' if i > 0 else ''
                extract = '(dw[elem_dw + %d] >> %d) & 0x%x' % (rel_dw, start, mask)

                if ftype == 'bool':
                    print('         fprintf(fp, "%s.%s = %%s", %s ? "true" : "false");' %
                          (sep, name, extract))
                elif ftype == 'address':
                    if start > 0:
                        addr_mask = (1 << start) - 1
                        print('         fprintf(fp, "%s.%s = 0x%%lx", '
                              '(unsigned long)(((uint64_t)dw[elem_dw + %d] | '
                              '((uint64_t)dw[elem_dw + %d] << 32)) & ~0x%xUL));' %
                              (sep, name, rel_dw, rel_dw + 1, addr_mask))
                    else:
                        print('         fprintf(fp, "%s.%s = 0x%%lx", '
                              '(unsigned long)((uint64_t)dw[elem_dw + %d] | '
                              '((uint64_t)dw[elem_dw + %d] << 32)));' %
                              (sep, name, rel_dw, rel_dw + 1))
                elif ftype == 'float':
                    print('         { union { uint32_t u; float f; } _fv = { .u = dw[elem_dw + %d] };' % rel_dw)
                    print('           fprintf(fp, "%s.%s = %%f", _fv.f); }' % (sep, name))
                elif values:
                    self._emit_enum_decode(extract, values, sep, name, indent='         ')
                elif width > 32:
                    print('         fprintf(fp, "%s.%s = 0x%%lx", '
                          '(unsigned long)((uint64_t)(dw[elem_dw + %d] >> %d) | '
                          '((uint64_t)dw[elem_dw + %d] << %d)));' %
                          (sep, name, rel_dw, start, rel_dw + 1, 32 - start))
                else:
                    print('         fprintf(fp, "%s.%s = %%u", %s);' %
                          (sep, name, extract))

            print('         fprintf(fp, "\\n");')

        print('      }')
        print('      for (unsigned i = group_tail; i < len; i++)')
        print('         fprintf(fp, "[0x%04x] 0x%08x\\n", base + i * 4, dw[i]);')
        print('   }')

    def emit_dispatch(self):
        """Emit command dispatch function.

        Uses the full opcode key and mask rather than just the top 16 bits
        so that instructions differing only in lower opcode fields
        (e.g. 3DPRIMITIVE vs 3DPRIMITIVE_EXTENDED) are dispatched correctly.
        Instructions with more specific masks (more default bits) are checked
        first so that a more general match does not shadow a specific one.
        """
        prefix = 'gfx%s' % self.gen

        print('static inline unsigned')
        print('%s_decode_command(FILE *fp, uint32_t offset, const uint32_t *dw, unsigned remaining)' % prefix)
        print('{')

        # Collect unique (key, mask) -> prefixed mappings.
        # Sort by popcount of mask descending so more specific matches
        # are tried first (e.g. 3DPRIMITIVE_EXTENDED before 3DPRIMITIVE).
        entries = []
        seen = set()
        for prefixed, opcode_key, opcode_mask, length, bias, fields_by_dw, variable_groups in self.instructions:
            if opcode_key == 0 or (opcode_key, opcode_mask) in seen:
                continue
            seen.add((opcode_key, opcode_mask))
            entries.append((opcode_key, opcode_mask, prefixed))

        entries.sort(key=lambda e: -bin(e[1]).count('1'))

        first = True
        for key, mask, prefixed in entries:
            keyword = 'if' if first else '} else if'
            print('   %s ((dw[0] & 0x%08xu) == 0x%08xu) {' % (keyword, mask, key))
            print('      return %s_decode(fp, offset, dw, remaining);' % prefixed)
            first = False

        if not first:
            print('   } else {')
        print('      return 0;')
        if not first:
            print('   }')
        print('}')
        print('')

    def emit_batch_decode(self):
        """Emit the batch buffer walker."""
        prefix = 'gfx%s' % self.gen

        print('static inline void')
        print('%s_decode_batch(FILE *fp, const uint32_t *batch, unsigned batch_dwords)' % prefix)
        print('{')
        print('   unsigned offset = 0;')
        print('')
        print('   while (offset < batch_dwords * 4) {')
        print('      const uint32_t *dw = &batch[offset / 4];')
        print('      uint32_t cmd = dw[0];')
        print('      unsigned len;')
        print('')
        print('      /* MI_BATCH_BUFFER_END */')
        print('      if (cmd == 0x05000000) {')
        print('         fprintf(fp, "[0x%04x] 0x%08x  MI_BATCH_BUFFER_END\\n", offset, cmd);')
        print('         break;')
        print('      }')
        print('')
        print('      /* MI_NOOP: opcode is all zeros, single dword */')
        print('      if (cmd == 0) {')
        print('         fprintf(fp, "[0x%04x] 0x%08x  MI_NOOP\\n", offset, cmd);')
        print('         offset += 4;')
        print('         continue;')
        print('      }')
        print('')
        print('      len = %s_decode_command(fp, offset, dw, batch_dwords - offset / 4);' % prefix)
        print('      if (!len) {')
        print('         fprintf(fp, "[0x%04x] 0x%08x  UNKNOWN\\n", offset, dw[0]);')
        print('         len = 1;')
        print('      }')
        print('      offset += len * 4;')
        print('   }')
        print('}')

    def generate(self):
        self.emit_header()
        for inst in self.instructions:
            self.emit_decode_function(*inst)
        self.emit_dispatch()
        self.emit_batch_decode()
        self.emit_footer()


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('xml_source', metavar='XML_SOURCE')
    p.add_argument('--engines', type=str, default='render',
                   help="Comma-separated engine list")
    p.add_argument('--baseline', type=str, action='append', default=[],
                   help="Previous gen XML files (oldest first); skip identical instructions")
    return p.parse_args()


def main():
    pargs = parse_args()
    engines = set(pargs.engines.split(','))

    genxml = intel_genxml.GenXml(pargs.xml_source)
    genxml.merge_imported()
    genxml.filter_engines(engines)

    root = genxml.et.getroot()
    platform = root.attrib['name']
    gen = root.attrib['gen'].replace('.', '')

    # Collect struct and enum definitions
    struct_defs = {}
    enum_defs = {}
    for item in root:
        if item.tag == 'struct':
            struct_defs[item.attrib['name']] = item
        elif item.tag == 'enum':
            enum_defs[item.attrib['name']] = item

    # Optional baseline filtering
    baseline_fps = {}
    if pargs.baseline:
        from gen_pack_header import _build_baselines, _item_fingerprint
        baseline_fps, _, _ = _build_baselines(pargs.baseline, engines)

    decoder = DecodeGenerator(gen, platform, struct_defs, enum_defs)

    for item in root:
        if item.tag != 'instruction':
            continue
        item_name = item.attrib['name']

        if baseline_fps:
            from gen_pack_header import _item_fingerprint
            fp = baseline_fps.get(item_name)
            if fp is not None and _item_fingerprint(item) == fp:
                continue

        decoder.add_instruction(item)

    decoder.generate()


if __name__ == '__main__':
    main()
