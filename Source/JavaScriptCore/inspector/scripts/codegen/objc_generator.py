#!/usr/bin/env python
#
# Copyright (c) 2014 Apple Inc. All rights reserved.
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
try:
    from .generator import Generator, ucfirst
    from .models import PrimitiveType, ObjectType, ArrayType, EnumType, AliasedType, Frameworks
except ValueError:
    from generator import Generator, ucfirst
    from models import PrimitiveType, ObjectType, ArrayType, EnumType, AliasedType, Frameworks

log = logging.getLogger('global')


def join_type_and_name(type_str, name_str):
    if type_str.endswith('*'):
        return type_str + name_str
    return type_str + ' ' + name_str


def strip_block_comment_markers(str):
    return str.replace('/*', '').replace('*/', '')


def remove_duplicate_from_str(str, possible_duplicate):
    return str.replace(possible_duplicate + possible_duplicate, possible_duplicate)


_OBJC_IDENTIFIER_RENAME_MAP = {
    'this': 'thisObject',  # Debugger.CallFrame.this
    'description': 'stringRepresentation',  # Runtime.RemoteObject.description
    'id': 'identifier',  # Page.Frame.id, Runtime.ExecutionContextDescription.id, Debugger.BreakpointAction.id
}

_OBJC_IDENTIFIER_REVERSE_RENAME_MAP = dict((v, k) for k, v in _OBJC_IDENTIFIER_RENAME_MAP.items())


class ObjCTypeCategory:
    Simple = 0
    String = 1
    Object = 2
    Array = 3

    @staticmethod
    def category_for_type(_type):
        if (isinstance(_type, PrimitiveType)):
            if _type.raw_name() is 'string':
                return ObjCTypeCategory.String
            if  _type.raw_name() in ['object', 'any']:
                return ObjCTypeCategory.Object
            if _type.raw_name() is 'array':
                return ObjCTypeCategory.Array
            return ObjCTypeCategory.Simple
        if (isinstance(_type, ObjectType)):
            return ObjCTypeCategory.Object
        if (isinstance(_type, ArrayType)):
            return ObjCTypeCategory.Array
        if (isinstance(_type, AliasedType)):
            return ObjCTypeCategory.category_for_type(_type.aliased_type)
        if (isinstance(_type, EnumType)):
            return ObjCTypeCategory.category_for_type(_type.primitive_type)
        return None



# This class contains extra static methods used for generation, but does
# not participate in any inheritance hierarchy. File generators should
# extend the generic "Generator" class instead.
class ObjCGenerator:
    OBJC_PREFIX = 'RWIProtocol'
    OBJC_JSON_OBJECT_BASE = '%sJSONObject' % OBJC_PREFIX

    # Adjust identifier names that collide with ObjC keywords.

    @staticmethod
    def identifier_to_objc_identifier(name):
        return _OBJC_IDENTIFIER_RENAME_MAP.get(name, name)

    @staticmethod
    def objc_identifier_to_identifier(name):
        return _OBJC_IDENTIFIER_REVERSE_RENAME_MAP.get(name, name)

    # Generate ObjC types, command handlers, and event dispatchers for a subset of domains.

    DOMAINS_TO_GENERATE = ['CSS', 'DOM', 'DOMStorage', 'Network', 'Page', 'GenericTypes']

    @staticmethod
    def should_generate_domain_types_filter(model):
        def should_generate_domain_types(domain):
            if model.framework is Frameworks.Test:
                return True
            whitelist = set(ObjCGenerator.DOMAINS_TO_GENERATE)
            whitelist.update(set(['Console', 'Debugger', 'Runtime']))
            return domain.domain_name in whitelist
        return should_generate_domain_types

    @staticmethod
    def should_generate_domain_command_handler_filter(model):
        def should_generate_domain_command_handler(domain):
            if model.framework is Frameworks.Test:
                return True
            whitelist = set(ObjCGenerator.DOMAINS_TO_GENERATE)
            return domain.domain_name in whitelist
        return should_generate_domain_command_handler

    @staticmethod
    def should_generate_domain_event_dispatcher_filter(model):
        def should_generate_domain_event_dispatcher(domain):
            if model.framework is Frameworks.Test:
                return True
            whitelist = set(ObjCGenerator.DOMAINS_TO_GENERATE)
            whitelist.add('Console')
            return domain.domain_name in whitelist
        return should_generate_domain_event_dispatcher

    # ObjC enum and type names.

    @staticmethod
    def objc_name_for_type(type):
        name = type.qualified_name().replace('.', '')
        name = remove_duplicate_from_str(name, type.type_domain().domain_name)
        return '%s%s' % (ObjCGenerator.OBJC_PREFIX, name)

    @staticmethod
    def objc_enum_name_for_anonymous_enum_declaration(declaration):
        domain_name = declaration.type.type_domain().domain_name
        name = '%s%s' % (domain_name, declaration.type.raw_name())
        name = remove_duplicate_from_str(name, domain_name)
        return '%s%s' % (ObjCGenerator.OBJC_PREFIX, name)

    @staticmethod
    def objc_enum_name_for_anonymous_enum_member(declaration, member):
        domain_name = member.type.type_domain().domain_name
        name = '%s%s%s' % (domain_name, declaration.type.raw_name(), ucfirst(member.member_name))
        name = remove_duplicate_from_str(name, domain_name)
        return '%s%s' % (ObjCGenerator.OBJC_PREFIX, name)

    @staticmethod
    def objc_enum_name_for_anonymous_enum_parameter(domain, event_or_command_name, parameter):
        domain_name = domain.domain_name
        name = '%s%s%s' % (domain_name, ucfirst(event_or_command_name), ucfirst(parameter.parameter_name))
        name = remove_duplicate_from_str(name, domain_name)
        return '%s%s' % (ObjCGenerator.OBJC_PREFIX, name)

    @staticmethod
    def objc_enum_name_for_non_anonymous_enum(_type):
        domain_name = _type.type_domain().domain_name
        name = _type.qualified_name().replace('.', '')
        name = remove_duplicate_from_str(name, domain_name)
        return '%s%s' % (ObjCGenerator.OBJC_PREFIX, name)

    # Miscellaneous name handling.

    @staticmethod
    def variable_name_prefix_for_domain(domain):
        domain_name = domain.domain_name
        if domain_name.startswith('DOM'):
            return 'dom' + domain_name[3:]
        if domain_name.startswith('CSS'):
            return 'css' + domain_name[3:]
        return domain_name[:1].lower() + domain_name[1:]

    # Type basics.

    @staticmethod
    def objc_accessor_type_for_raw_name(raw_name):
        if raw_name in ['string', 'array']:
            return 'copy'
        if raw_name in ['integer', 'number', 'boolean']:
            return 'assign'
        if raw_name in ['any', 'object']:
            return 'retain'
        return None

    @staticmethod
    def objc_type_for_raw_name(raw_name):
        if raw_name is 'string':
            return 'NSString *'
        if raw_name is 'array':
            return 'NSArray *'
        if raw_name is 'integer':
            return 'int'
        if raw_name is 'number':
            return 'double'
        if raw_name is 'boolean':
            return 'BOOL'
        if raw_name in ['any', 'object']:
            return '%s *' % ObjCGenerator.OBJC_JSON_OBJECT_BASE
        return None

    @staticmethod
    def objc_class_for_raw_name(raw_name):
        if raw_name is 'string':
            return 'NSString'
        if raw_name is 'array':
            return 'NSArray'
        if raw_name in ['integer', 'number', 'boolean']:
            return 'NSNumber'
        if raw_name in ['any', 'object']:
            return ObjCGenerator.OBJC_JSON_OBJECT_BASE
        return None

    # FIXME: Can these protocol_type functions be removed in favor of C++ generators functions?

    @staticmethod
    def protocol_type_for_raw_name(raw_name):
        if raw_name is 'string':
            return 'String'
        if raw_name is 'integer':
            return 'int'
        if raw_name is 'number':
            return 'double'
        if raw_name is 'boolean':
            return 'bool'
        if raw_name in ['any', 'object']:
            return 'InspectorObject'
        return None

    @staticmethod
    def protocol_type_for_type(_type):
        if (isinstance(_type, AliasedType)):
            _type = _type.aliased_type
        if (isinstance(_type, PrimitiveType)):
            return ObjCGenerator.protocol_type_for_raw_name(_type.raw_name())
        if (isinstance(_type, EnumType)):
            return ObjCGenerator.protocol_type_for_type(_type.primitive_type)
        if (isinstance(_type, ObjectType)):
            return 'Inspector::Protocol::%s::%s' % (_type.type_domain().domain_name, _type.raw_name())
        if (isinstance(_type, ArrayType)):
            sub_type = ObjCGenerator.protocol_type_for_type(_type.element_type)
            return 'Inspector::Protocol::Array<%s>' % sub_type
        return None

    @staticmethod
    def is_type_objc_pointer_type(_type):
        if (isinstance(_type, AliasedType)):
            _type = _type.aliased_type
        if (isinstance(_type, PrimitiveType)):
            return _type.raw_name() in ['string', 'array', 'any', 'object']
        if (isinstance(_type, EnumType)):
            return False
        if (isinstance(_type, ObjectType)):
            return True
        if (isinstance(_type, ArrayType)):
            return True
        return None

    @staticmethod
    def objc_class_for_type(_type):
        if (isinstance(_type, AliasedType)):
            _type = _type.aliased_type
        if (isinstance(_type, PrimitiveType)):
            return ObjCGenerator.objc_class_for_raw_name(_type.raw_name())
        if (isinstance(_type, EnumType)):
            return ObjCGenerator.objc_class_for_raw_name(_type.primitive_type.raw_name())
        if (isinstance(_type, ObjectType)):
            return ObjCGenerator.objc_name_for_type(_type)
        if (isinstance(_type, ArrayType)):
            sub_type = strip_block_comment_markers(ObjCGenerator.objc_class_for_type(_type.element_type))
            return 'NSArray/*<%s>*/' % sub_type
        return None

    @staticmethod
    def objc_class_for_array_type(_type):
        if isinstance(_type, AliasedType):
            _type = _type.aliased_type
        if isinstance(_type, ArrayType):
            return ObjCGenerator.objc_class_for_type(_type.element_type)
        return None

    @staticmethod
    def objc_accessor_type_for_member(member):
        return ObjCGenerator.objc_accessor_type_for_member_internal(member.type)

    @staticmethod
    def objc_accessor_type_for_member_internal(_type):
        if (isinstance(_type, AliasedType)):
            _type = _type.aliased_type
        if (isinstance(_type, PrimitiveType)):
            return ObjCGenerator.objc_accessor_type_for_raw_name(_type.raw_name())
        if (isinstance(_type, EnumType)):
            return 'assign'
        if (isinstance(_type, ObjectType)):
            return 'retain'
        if (isinstance(_type, ArrayType)):
            return 'copy'
        return None

    @staticmethod
    def objc_type_for_member(declaration, member):
        return ObjCGenerator.objc_type_for_member_internal(member.type, declaration, member)

    @staticmethod
    def objc_type_for_member_internal(_type, declaration, member):
        if (isinstance(_type, AliasedType)):
            _type = _type.aliased_type
        if (isinstance(_type, PrimitiveType)):
            return ObjCGenerator.objc_type_for_raw_name(_type.raw_name())
        if (isinstance(_type, EnumType)):
            if (_type.is_anonymous):
                return ObjCGenerator.objc_enum_name_for_anonymous_enum_member(declaration, member)
            return ObjCGenerator.objc_enum_name_for_non_anonymous_enum(_type)
        if (isinstance(_type, ObjectType)):
            return ObjCGenerator.objc_name_for_type(_type) + ' *'
        if (isinstance(_type, ArrayType)):
            sub_type = strip_block_comment_markers(ObjCGenerator.objc_class_for_type(_type.element_type))
            return 'NSArray/*<%s>*/ *' % sub_type
        return None

    @staticmethod
    def objc_type_for_param(domain, event_or_command_name, parameter, respect_optional=True):
        objc_type = ObjCGenerator.objc_type_for_param_internal(parameter.type, domain, event_or_command_name, parameter)
        if respect_optional and parameter.is_optional:
            if objc_type.endswith('*'):
                return objc_type + '*'
            return objc_type + ' *'
        return objc_type

    @staticmethod
    def objc_type_for_param_internal(_type, domain, event_or_command_name, parameter):
        if (isinstance(_type, AliasedType)):
            _type = _type.aliased_type
        if (isinstance(_type, PrimitiveType)):
            return ObjCGenerator.objc_type_for_raw_name(_type.raw_name())
        if (isinstance(_type, EnumType)):
            if _type.is_anonymous:
                return ObjCGenerator.objc_enum_name_for_anonymous_enum_parameter(domain, event_or_command_name, parameter)
            return ObjCGenerator.objc_enum_name_for_non_anonymous_enum(_type)
        if (isinstance(_type, ObjectType)):
            return ObjCGenerator.objc_name_for_type(_type) + ' *'
        if (isinstance(_type, ArrayType)):
            sub_type = strip_block_comment_markers(ObjCGenerator.objc_class_for_type(_type.element_type))
            return 'NSArray/*<%s>*/ *' % sub_type
        return None

    # ObjC <-> Protocol conversion for commands and events.
    #   - convert a command call parameter received from Protocol to ObjC for handler
    #   - convert a command return parameter in callback block from ObjC to Protocol to send
    #   - convert an event parameter from ObjC API to Protocol to send

    @staticmethod
    def objc_protocol_export_expression_for_variable(var_type, var_name):
        category = ObjCTypeCategory.category_for_type(var_type)
        if category in [ObjCTypeCategory.Simple, ObjCTypeCategory.String]:
            if isinstance(var_type, EnumType):
                return 'toProtocolString(%s)' % var_name
            return var_name
        if category is ObjCTypeCategory.Object:
            return '[%s toInspectorObject]' % var_name
        if category is ObjCTypeCategory.Array:
            protocol_type = ObjCGenerator.protocol_type_for_type(var_type.element_type)
            objc_class = ObjCGenerator.objc_class_for_type(var_type.element_type)
            if protocol_type == 'Inspector::Protocol::Array<String>':
                return 'inspectorStringArrayArray(%s)' % var_name
            if protocol_type is 'String' and objc_class is 'NSString':
                return 'inspectorStringArray(%s)' % var_name
            if protocol_type is 'int' and objc_class is 'NSNumber':
                return 'inspectorIntegerArray(%s)' % var_name
            if protocol_type is 'double' and objc_class is 'NSNumber':
                return 'inspectorDoubleArray(%s)' % var_name
            return 'inspectorObjectArray(%s)' % var_name

    @staticmethod
    def objc_protocol_import_expression_for_member(name, declaration, member):
        if isinstance(member.type, EnumType):
            if member.type.is_anonymous:
                return 'fromProtocolString<%s>(%s)' % (ObjCGenerator.objc_enum_name_for_anonymous_enum_member(declaration, member), name)
            return 'fromProtocolString<%s>(%s)' % (ObjCGenerator.objc_enum_name_for_non_anonymous_enum(member.type), name)
        return ObjCGenerator.objc_protocol_import_expression_for_variable(member.type, name)

    @staticmethod
    def objc_protocol_import_expression_for_parameter(name, domain, event_or_command_name, parameter):
        if isinstance(parameter.type, EnumType):
            if parameter.type.is_anonymous:
                return 'fromProtocolString<%s>(%s)' % (ObjCGenerator.objc_enum_name_for_anonymous_enum_parameter(domain, event_or_command_name, parameter), name)
            return 'fromProtocolString<%s>(%s)' % (ObjCGenerator.objc_enum_name_for_non_anonymous_enum(parameter.type), name)
        return ObjCGenerator.objc_protocol_import_expression_for_variable(parameter.type, name)

    @staticmethod
    def objc_protocol_import_expression_for_variable(var_type, var_name):
        category = ObjCTypeCategory.category_for_type(var_type)
        if category in [ObjCTypeCategory.Simple, ObjCTypeCategory.String]:
            return var_name
        if category is ObjCTypeCategory.Object:
            objc_class = ObjCGenerator.objc_class_for_type(var_type)
            return '[[[%s alloc] initWithInspectorObject:%s] autorelease]' % (objc_class, var_name)
        if category is ObjCTypeCategory.Array:
            objc_class = ObjCGenerator.objc_class_for_type(var_type.element_type)
            if objc_class is 'NSString':
                return 'objcStringArray(%s)' % var_name
            if objc_class is 'NSNumber':  # FIXME: Integer or Double?
                return 'objcIntegerArray(%s)' % var_name
            return 'objcArray<%s>(%s)' % (objc_class, var_name)

    # ObjC <-> JSON object conversion for types getters/setters.
    #   - convert a member setter from ObjC API to JSON object setter
    #   - convert a member getter from JSON object to ObjC API

    @staticmethod
    def objc_to_protocol_expression_for_member(declaration, member, sub_expression):
        category = ObjCTypeCategory.category_for_type(member.type)
        if category in [ObjCTypeCategory.Simple, ObjCTypeCategory.String]:
            if isinstance(member.type, EnumType):
                return 'toProtocolString(%s)' % sub_expression
            return sub_expression
        if category is ObjCTypeCategory.Object:
            return sub_expression
        if category is ObjCTypeCategory.Array:
            objc_class = ObjCGenerator.objc_class_for_type(member.type.element_type)
            if objc_class is 'NSString':
                return 'inspectorStringArray(%s)' % sub_expression
            if objc_class is 'NSNumber':
                protocol_type = ObjCGenerator.protocol_type_for_type(member.type.element_type)
                if protocol_type is 'double':
                    return 'inspectorDoubleArray(%s)' % sub_expression
                return 'inspectorIntegerArray(%s)' % sub_expression
            return 'inspectorObjectArray(%s)' % sub_expression

    @staticmethod
    def protocol_to_objc_expression_for_member(declaration, member, sub_expression):
        category = ObjCTypeCategory.category_for_type(member.type)
        if category in [ObjCTypeCategory.Simple, ObjCTypeCategory.String]:
            if isinstance(member.type, EnumType):
                if member.type.is_anonymous:
                    return 'fromProtocolString<%s>(%s)' % (ObjCGenerator.objc_enum_name_for_anonymous_enum_member(declaration, member), sub_expression)
                return 'fromProtocolString<%s>(%s)' % (ObjCGenerator.objc_enum_name_for_non_anonymous_enum(member.type), sub_expression)
            return sub_expression
        if category is ObjCTypeCategory.Object:
            objc_type = ObjCGenerator.objc_type_for_member(declaration, member)
            return '(%s)%s' % (objc_type, sub_expression)
        if category is ObjCTypeCategory.Array:
            protocol_type = ObjCGenerator.protocol_type_for_type(member.type.element_type)
            objc_class = ObjCGenerator.objc_class_for_type(member.type.element_type)
            if objc_class is 'NSString':
                return 'objcStringArray(%s)' % sub_expression
            if objc_class is 'NSNumber':
                protocol_type = ObjCGenerator.protocol_type_for_type(member.type.element_type)
                if protocol_type is 'double':
                    return 'objcDoubleArray(%s)' % sub_expression
                return 'objcIntegerArray(%s)' % sub_expression
            return 'objcArray<%s>(%s)' % (objc_class, sub_expression)

    # JSON object setter/getter selectors for types.

    @staticmethod
    def objc_setter_method_for_member(declaration, member):
        return ObjCGenerator.objc_setter_method_for_member_internal(member.type, declaration, member)

    @staticmethod
    def objc_setter_method_for_member_internal(_type, declaration, member):
        if (isinstance(_type, AliasedType)):
            _type = _type.aliased_type
        if (isinstance(_type, PrimitiveType)):
            raw_name = _type.raw_name()
            if raw_name is 'boolean':
                return 'setBool'
            if raw_name is 'integer':
                return 'setInteger'
            if raw_name is 'number':
                return 'setDouble'
            if raw_name is 'string':
                return 'setString'
            if raw_name in ['any', 'object']:
                return 'setObject'
            if raw_name is 'array':
                return 'setInspectorArray'
            return None
        if (isinstance(_type, EnumType)):
            return 'setString'
        if (isinstance(_type, ObjectType)):
            return 'setObject'
        if (isinstance(_type, ArrayType)):
            return 'setInspectorArray'
        return None

    @staticmethod
    def objc_getter_method_for_member(declaration, member):
        return ObjCGenerator.objc_getter_method_for_member_internal(member.type, declaration, member)

    @staticmethod
    def objc_getter_method_for_member_internal(_type, declaration, member):
        if (isinstance(_type, AliasedType)):
            _type = _type.aliased_type
        if (isinstance(_type, PrimitiveType)):
            raw_name = _type.raw_name()
            if raw_name is 'boolean':
                return 'boolForKey'
            if raw_name is 'integer':
                return 'integerForKey'
            if raw_name is 'number':
                return 'doubleForKey'
            if raw_name is 'string':
                return 'stringForKey'
            if raw_name in ['any', 'object']:
                return 'objectForKey'
            if raw_name is 'array':
                return 'inspectorArrayForKey'
            return None
        if (isinstance(_type, EnumType)):
            return 'stringForKey'
        if (isinstance(_type, ObjectType)):
            return 'objectForKey'
        if (isinstance(_type, ArrayType)):
            return 'inspectorArrayForKey'
        return None
