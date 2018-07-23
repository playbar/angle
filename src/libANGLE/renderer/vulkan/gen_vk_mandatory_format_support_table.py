#!/usr/bin/python
# Copyright 2018 The ANGLE Project Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# gen_vk_mandatory_format_support_table.py:
#  Code generation for mandatory formats supported by Vulkan.

from datetime import date
import sys

sys.path.append('..')
import angle_format
import xml.etree.ElementTree as etree
import sys, os


template_table_autogen_cpp = """// GENERATED FILE - DO NOT EDIT.
// Generated by {script_name} using data from {input_file_name} and
// the vk.xml file situated at
// /third_party/vulkan-validation-layers/src/scripts/vk.xml
//
// Copyright {copyright_year} The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// {out_file_name}:
//   Queries for full Vulkan mandatory format support information based on VK format.

#include "libANGLE/renderer/vulkan/vk_format_utils.h"

using namespace angle;

namespace
{{
constexpr std::array<VkFormatProperties, {num_formats}> kFormatProperties = {{{{
    {format_case_data}
}}}};
}}  // anonymous namespace

namespace rx
{{

namespace vk
{{

const VkFormatProperties& GetMandatoryFormatSupport(VkFormat vkFormat)
{{
    ASSERT(static_cast<uint64_t>(vkFormat) < sizeof(kFormatProperties));
    return kFormatProperties[vkFormat];
}}

}}  // namespace vk

}}  // namespace rx

"""

template_format_property = """
/* {vk_format} */
{{{{}}, {optimal_features}, {buffer_features}}}"""


def script_relative(path):
    return os.path.join(os.path.dirname(sys.argv[0]), path)


def gen_format_case(index, vk_to_index_to_format_map, vk_map):
    vk_format = vk_to_index_to_format_map[index]

    if vk_format in vk_map and len(vk_map[vk_format]) > 0:
        # Check which feature is a buffer feature or not.
        buffer_features = [x for x in vk_map[vk_format] if x.find("_BUFFER_") != -1]
        optimal_features = [x for x in vk_map[vk_format] if x.find("_BUFFER_") == -1]
        optimal_features_str = "|".join(optimal_features) if len(optimal_features) else "0"
        buffer_features_str = "|".join(buffer_features) if len(buffer_features) else "0"
    else:
        optimal_features_str = "0"
        buffer_features_str = "0"

    return template_format_property.format(
        vk_format = vk_format,
        optimal_features = optimal_features_str,
        buffer_features = buffer_features_str)


input_file_name = 'vk_mandatory_format_support_data.json'
out_file_name = 'vk_mandatory_format_support_table'

tree = etree.parse(script_relative('../../../../third_party/vulkan-headers/src/registry/vk.xml'))
root = tree.getroot()
vk_format_enums = root.findall(".//enums[@name='VkFormat']/enum")
vk_format_name_to_index_map = {}
num_formats = len(vk_format_enums)
for format_enum in vk_format_enums:
    index = int(format_enum.attrib['value'])
    vk_format = format_enum.attrib['name']
    vk_format_name_to_index_map[index] = vk_format

vk_map = angle_format.load_json(input_file_name)
vk_cases = [gen_format_case(index, vk_format_name_to_index_map, vk_map) for index in vk_format_name_to_index_map]

output_cpp = template_table_autogen_cpp.format(
    copyright_year = date.today().year,
    num_formats = num_formats,
    format_case_data = "\n,".join(vk_cases),
    script_name = __file__,
    out_file_name = out_file_name,
    input_file_name = input_file_name)

with open(out_file_name + '_autogen.cpp', 'wt') as out_file:
    out_file.write(output_cpp)
    out_file.close()
