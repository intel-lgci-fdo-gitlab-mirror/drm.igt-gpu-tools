#encoding=utf-8
# SPDX-License-Identifier: MIT
#
# Copyright © 2020 Intel Corporation

# A few utility functions reused across genxml scripts

import re

alphanum_nono = re.compile(r'[ /\[\]()\-:.,=>#&*\'"+\\]+')
def to_alphanum(name):
    global alphanum_nono
    return alphanum_nono.sub('', name)

def safe_name(name):
    name = to_alphanum(name)
    if not name[0].isalpha():
        name = '_' + name
    return name
