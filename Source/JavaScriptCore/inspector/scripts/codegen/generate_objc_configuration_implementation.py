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
    from .generator import Generator
    from .objc_generator import ObjCGenerator
    from .objc_generator_templates import ObjCGeneratorTemplates as ObjCTemplates
except ValueError:
    from generator import Generator
    from objc_generator import ObjCGenerator
    from objc_generator_templates import ObjCGeneratorTemplates as ObjCTemplates

log = logging.getLogger('global')


class ObjCBackendDispatcherImplementationGenerator(Generator):
    def __init__(self, model, input_filepath):
        Generator.__init__(self, model, input_filepath)

    def output_filename(self):
        return '%sConfiguration.mm' % ObjCGenerator.OBJC_PREFIX

    def generate_output(self):
        secondary_headers = [
            '"%sInternal.h"' % ObjCGenerator.OBJC_PREFIX,
            '"%sBackendDispatchers.h"' % ObjCGenerator.OBJC_PREFIX,
            '<JavaScriptCore/AlternateDispatchableAgent.h>',
            '<JavaScriptCore/AugmentableInspectorController.h>',
            '<JavaScriptCore/InspectorAlternateBackendDispatchers.h>',
            '<JavaScriptCore/InspectorBackendDispatchers.h>',
        ]

        header_args = {
            'primaryInclude': '"%sConfiguration.h"' % ObjCGenerator.OBJC_PREFIX,
            'secondaryIncludes': '\n'.join(['#import %s' % header for header in secondary_headers]),
        }

        self._command_filter = ObjCGenerator.should_generate_domain_command_handler_filter(self.model())
        self._event_filter = ObjCGenerator.should_generate_domain_event_dispatcher_filter(self.model())

        domains = self.domains_to_generate()
        sections = []
        sections.append(self.generate_license())
        sections.append(Template(ObjCTemplates.ImplementationPrelude).substitute(None, **header_args))
        sections.append(self._generate_configuration_implementation_for_domains(domains))
        sections.append(Template(ObjCTemplates.ImplementationPostlude).substitute(None, **header_args))
        return '\n\n'.join(sections)

    def _generate_configuration_implementation_for_domains(self, domains):
        lines = []
        lines.append('@implementation %sConfiguration' % ObjCGenerator.OBJC_PREFIX)
        lines.append('{')
        lines.append('    AugmentableInspectorController* _controller;')
        lines.extend(self._generate_ivars(domains))
        lines.append('}')
        lines.append('')
        lines.append('- (instancetype)initWithController:(AugmentableInspectorController*)controller')
        lines.append('{')
        lines.append('    self = [super init];')
        lines.append('    if (!self)')
        lines.append('        return nil;')
        lines.append('    ASSERT(controller);')
        lines.append('    _controller = controller;')
        lines.append('    return self;')
        lines.append('}')
        lines.append('')
        lines.extend(self._generate_dealloc(domains))
        lines.append('')
        for domain in domains:
            if domain.commands and self._command_filter(domain):
                lines.append(self._generate_handler_setter_for_domain(domain))
                lines.append('')
            if domain.events and self._event_filter(domain):
                lines.append(self._generate_event_dispatcher_getter_for_domain(domain))
                lines.append('')
        lines.append('@end')
        return '\n'.join(lines)

    def _generate_ivars(self, domains):
        lines = []
        for domain in domains:
            if domain.commands and self._command_filter(domain):
                objc_class_name = '%s%sDomainHandler' % (ObjCGenerator.OBJC_PREFIX, domain.domain_name)
                ivar_name = '_%sHandler' % ObjCGenerator.variable_name_prefix_for_domain(domain)
                lines.append('    id<%s> %s;' % (objc_class_name, ivar_name))
            if domain.events and self._event_filter(domain):
                objc_class_name = '%s%sDomainEventDispatcher' % (ObjCGenerator.OBJC_PREFIX, domain.domain_name)
                ivar_name = '_%sEventDispatcher' % ObjCGenerator.variable_name_prefix_for_domain(domain)
                lines.append('    %s *%s;' % (objc_class_name, ivar_name))
        return lines

    def _generate_dealloc(self, domains):
        lines = []
        lines.append('- (void)dealloc')
        lines.append('{')
        for domain in domains:
            if domain.commands and self._command_filter(domain):
                lines.append('    [_%sHandler release];' % ObjCGenerator.variable_name_prefix_for_domain(domain))
            if domain.events and self._event_filter(domain):
                lines.append('    [_%sEventDispatcher release];' % ObjCGenerator.variable_name_prefix_for_domain(domain))
        lines.append('    [super dealloc];')
        lines.append('}')
        return lines

    def _generate_handler_setter_for_domain(self, domain):
        setter_args = {
            'objcPrefix': ObjCGenerator.OBJC_PREFIX,
            'domainName': domain.domain_name,
            'variableNamePrefix': ObjCGenerator.variable_name_prefix_for_domain(domain),
        }
        return Template(ObjCTemplates.ConfigurationCommandPropertyImplementation).substitute(None, **setter_args)

    def _generate_event_dispatcher_getter_for_domain(self, domain):
        getter_args = {
            'objcPrefix': ObjCGenerator.OBJC_PREFIX,
            'domainName': domain.domain_name,
            'variableNamePrefix': ObjCGenerator.variable_name_prefix_for_domain(domain),
        }
        return Template(ObjCTemplates.ConfigurationGetterImplementation).substitute(None, **getter_args)

    def _variable_name_prefix_for_domain(self, domain):
        domain_name = domain.domain_name
        if domain_name.startswith('DOM'):
            return 'dom' + domain_name[3:]
        if domain_name.startswith('CSS'):
            return 'css' + domain_name[3:]
        return domain_name[:1].lower() + domain_name[1:]
