#!/usr/bin/env python
#
# Copyright (c) 2014 Apple Inc. All rights reserved.
# Copyright (c) 2014 University of Washington. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
# THE POSSIBILITY OF SUCH DAMAGE.


import logging
import string
from string import Template

try:
    from .generator import Generator, ucfirst
    from .models import ObjectType
    from .objc_generator import ObjCTypeCategory, ObjCGenerator
    from .objc_generator_templates import ObjCGeneratorTemplates as ObjCTemplates
except ValueError:
    from generator import Generator, ucfirst
    from models import ObjectType
    from objc_generator import ObjCTypeCategory, ObjCGenerator
    from objc_generator_templates import ObjCGeneratorTemplates as ObjCTemplates

log = logging.getLogger('global')


def add_newline(lines):
    if lines and lines[-1] == '':
        return
    lines.append('')


class ObjCProtocolTypesImplementationGenerator(Generator):
    def __init__(self, model, input_filepath):
        Generator.__init__(self, model, input_filepath)

    def output_filename(self):
        return '%sTypes.mm' % ObjCGenerator.OBJC_PREFIX

    def domains_to_generate(self):
        return list(filter(ObjCGenerator.should_generate_domain_types_filter(self.model()), Generator.domains_to_generate(self)))

    def generate_output(self):
        secondary_headers = [
            '"%sEnumConversionHelpers.h"' % ObjCGenerator.OBJC_PREFIX,
            '<JavaScriptCore/InspectorValues.h>',
            '<wtf/Assertions.h>',
        ]

        header_args = {
            'primaryInclude': '"%sInternal.h"' % ObjCGenerator.OBJC_PREFIX,
            'secondaryIncludes': '\n'.join(['#import %s' % header for header in secondary_headers]),
        }

        domains = self.domains_to_generate()
        sections = []
        sections.append(self.generate_license())
        sections.append(Template(ObjCTemplates.ImplementationPrelude).substitute(None, **header_args))
        sections.extend(list(map(self.generate_type_implementations, domains)))
        sections.append(Template(ObjCTemplates.ImplementationPostlude).substitute(None, **header_args))
        return '\n\n'.join(sections)

    def generate_type_implementations(self, domain):
        lines = []
        for declaration in domain.type_declarations:
            if (isinstance(declaration.type, ObjectType)):
                add_newline(lines)
                lines.append(self.generate_type_implementation(domain, declaration))
        return '\n'.join(lines)

    def generate_type_implementation(self, domain, declaration):
        lines = []
        lines.append('@implementation %s' % ObjCGenerator.objc_name_for_type(declaration.type))
        required_members = [member for member in declaration.type_members if not member.is_optional]
        if required_members:
            lines.append('')
            lines.append(self._generate_init_method_for_required_members(domain, declaration, required_members))
        for member in declaration.type_members:
            lines.append('')
            lines.append(self._generate_setter_for_member(domain, declaration, member))
            lines.append('')
            lines.append(self._generate_getter_for_member(domain, declaration, member))
        lines.append('')
        lines.append('@end')
        return '\n'.join(lines)

    def _generate_init_method_for_required_members(self, domain, declaration, required_members):
        pairs = []
        for member in required_members:
            objc_type = ObjCGenerator.objc_type_for_member(declaration, member)
            var_name = ObjCGenerator.identifier_to_objc_identifier(member.member_name)
            pairs.append('%s:(%s)%s' % (var_name, objc_type, var_name))
        pairs[0] = ucfirst(pairs[0])
        lines = []
        lines.append('- (instancetype)initWith%s;' % ' '.join(pairs))
        lines.append('{')
        lines.append('    self = [super init];')
        lines.append('    if (!self)')
        lines.append('        return nil;')
        lines.append('')

        required_pointer_members = [member for member in required_members if ObjCGenerator.is_type_objc_pointer_type(member.type)]
        if required_pointer_members:
            for member in required_pointer_members:
                var_name = ObjCGenerator.identifier_to_objc_identifier(member.member_name)
                lines.append('    THROW_EXCEPTION_FOR_REQUIRED_PROPERTY(%s, @"%s");' % (var_name, var_name))
                objc_array_class = ObjCGenerator.objc_class_for_array_type(member.type)
                if objc_array_class and objc_array_class.startswith(ObjCGenerator.OBJC_PREFIX):
                    lines.append('    THROW_EXCEPTION_FOR_BAD_TYPE_IN_ARRAY(%s, [%s class]);' % (var_name, objc_array_class))
            lines.append('')

        for member in required_members:
            var_name = ObjCGenerator.identifier_to_objc_identifier(member.member_name)
            lines.append('    self.%s = %s;' % (var_name, var_name))

        lines.append('')
        lines.append('    return self;')
        lines.append('}')
        return '\n'.join(lines)

    def _generate_setter_for_member(self, domain, declaration, member):
        objc_type = ObjCGenerator.objc_type_for_member(declaration, member)
        var_name = ObjCGenerator.identifier_to_objc_identifier(member.member_name)
        setter_method = ObjCGenerator.objc_setter_method_for_member(declaration, member)
        conversion_expression = ObjCGenerator.objc_to_protocol_expression_for_member(declaration, member, var_name)
        lines = []
        lines.append('- (void)set%s:(%s)%s' % (ucfirst(var_name), objc_type, var_name))
        lines.append('{')
        objc_array_class = ObjCGenerator.objc_class_for_array_type(member.type)
        if objc_array_class and objc_array_class.startswith(ObjCGenerator.OBJC_PREFIX):
            lines.append('    THROW_EXCEPTION_FOR_BAD_TYPE_IN_ARRAY(%s, [%s class]);' % (var_name, objc_array_class))
        lines.append('    [super %s:%s forKey:@"%s"];' % (setter_method, conversion_expression, member.member_name))
        lines.append('}')
        return '\n'.join(lines)

    def _generate_getter_for_member(self, domain, declaration, member):
        objc_type = ObjCGenerator.objc_type_for_member(declaration, member)
        var_name = ObjCGenerator.identifier_to_objc_identifier(member.member_name)
        getter_method = ObjCGenerator.objc_getter_method_for_member(declaration, member)
        basic_expression = '[super %s:@"%s"]' % (getter_method, member.member_name)
        conversion_expression = ObjCGenerator.protocol_to_objc_expression_for_member(declaration, member, basic_expression)
        lines = []
        lines.append('- (%s)%s' % (objc_type, var_name))
        lines.append('{')
        lines.append('    return %s;' % conversion_expression)
        lines.append('}')
        return '\n'.join(lines)
