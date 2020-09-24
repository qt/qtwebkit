/*
 *  Copyright (C) 1999-2001 Harri Porten (porten@kde.org)
 *  Copyright (C) 2001 Peter Kelly (pmk@post.com)
 *  Copyright (C) 2003, 2006, 2007, 2008, 2009, 2010, 2013 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "Parser.h"

#include "ASTBuilder.h"
#include "CodeBlock.h"
#include "Debugger.h"
#include "JSCJSValueInlines.h"
#include "Lexer.h"
#include "JSCInlines.h"
#include "SetForScope.h"
#include "SourceProvider.h"
#include "VM.h"
#include <utility>
#include <wtf/HashFunctions.h>
#include <wtf/StringPrintStream.h>
#include <wtf/WTFThreadData.h>


#define updateErrorMessage(shouldPrintToken, ...) do {\
    propagateError(); \
    logError(shouldPrintToken, __VA_ARGS__); \
} while (0)

#define propagateError() do { if (hasError()) return 0; } while (0)
#define internalFailWithMessage(shouldPrintToken, ...) do { updateErrorMessage(shouldPrintToken, __VA_ARGS__); return 0; } while (0)
#define handleErrorToken() do { if (m_token.m_type == EOFTOK || m_token.m_type & ErrorTokenFlag) { failDueToUnexpectedToken(); } } while (0)
#define failWithMessage(...) do { { handleErrorToken(); updateErrorMessage(true, __VA_ARGS__); } return 0; } while (0)
#define failWithStackOverflow() do { updateErrorMessage(false, "Stack exhausted"); m_hasStackOverflow = true; return 0; } while (0)
#define failIfFalse(cond, ...) do { if (!(cond)) { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define failIfTrue(cond, ...) do { if (cond) { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define failIfTrueIfStrict(cond, ...) do { if ((cond) && strictMode()) internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define failIfFalseIfStrict(cond, ...) do { if ((!(cond)) && strictMode()) internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define consumeOrFail(tokenType, ...) do { if (!consume(tokenType)) { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define consumeOrFailWithFlags(tokenType, flags, ...) do { if (!consume(tokenType, flags)) { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define matchOrFail(tokenType, ...) do { if (!match(tokenType)) { handleErrorToken(); internalFailWithMessage(true, __VA_ARGS__); } } while (0)
#define failIfStackOverflow() do { if (!canRecurse()) failWithStackOverflow(); } while (0)
#define semanticFail(...) do { internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define semanticFailIfTrue(cond, ...) do { if (cond) internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define semanticFailIfFalse(cond, ...) do { if (!(cond)) internalFailWithMessage(false, __VA_ARGS__); } while (0)
#define regexFail(failure) do { setErrorMessage(failure); return 0; } while (0)
#define restoreSavePointAndFail(savePoint, message) do { restoreSavePointWithError(savePoint, message); return 0; } while (0)
#define failDueToUnexpectedToken() do {\
        logError(true);\
    return 0;\
} while (0)

#define handleProductionOrFail(token, tokenString, operation, production) do {\
    consumeOrFail(token, "Expected '", tokenString, "' to ", operation, " a ", production);\
} while (0)

#define semanticFailureDueToKeyword(...) do { \
    if (strictMode() && m_token.m_type == RESERVED_IF_STRICT) \
        semanticFail("Cannot use the reserved word '", getToken(), "' as a ", __VA_ARGS__, " in strict mode"); \
    if (m_token.m_type == RESERVED || m_token.m_type == RESERVED_IF_STRICT) \
        semanticFail("Cannot use the reserved word '", getToken(), "' as a ", __VA_ARGS__); \
    if (m_token.m_type & KeywordTokenFlag) \
        semanticFail("Cannot use the keyword '", getToken(), "' as a ", __VA_ARGS__); \
} while (0)

using namespace std;

namespace JSC {

template <typename LexerType>
void Parser<LexerType>::logError(bool)
{
    if (hasError())
        return;
    StringPrintStream stream;
    printUnexpectedTokenText(stream);
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType> template <typename A>
void Parser<LexerType>::logError(bool shouldPrintToken, const A& value1)
{
    if (hasError())
        return;
    StringPrintStream stream;
    if (shouldPrintToken) {
        printUnexpectedTokenText(stream);
        stream.print(". ");
    }
    stream.print(value1, ".");
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType> template <typename A, typename B>
void Parser<LexerType>::logError(bool shouldPrintToken, const A& value1, const B& value2)
{
    if (hasError())
        return;
    StringPrintStream stream;
    if (shouldPrintToken) {
        printUnexpectedTokenText(stream);
        stream.print(". ");
    }
    stream.print(value1, value2, ".");
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType> template <typename A, typename B, typename C>
void Parser<LexerType>::logError(bool shouldPrintToken, const A& value1, const B& value2, const C& value3)
{
    if (hasError())
        return;
    StringPrintStream stream;
    if (shouldPrintToken) {
        printUnexpectedTokenText(stream);
        stream.print(". ");
    }
    stream.print(value1, value2, value3, ".");
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType> template <typename A, typename B, typename C, typename D>
void Parser<LexerType>::logError(bool shouldPrintToken, const A& value1, const B& value2, const C& value3, const D& value4)
{
    if (hasError())
        return;
    StringPrintStream stream;
    if (shouldPrintToken) {
        printUnexpectedTokenText(stream);
        stream.print(". ");
    }
    stream.print(value1, value2, value3, value4, ".");
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType> template <typename A, typename B, typename C, typename D, typename E>
void Parser<LexerType>::logError(bool shouldPrintToken, const A& value1, const B& value2, const C& value3, const D& value4, const E& value5)
{
    if (hasError())
        return;
    StringPrintStream stream;
    if (shouldPrintToken) {
        printUnexpectedTokenText(stream);
        stream.print(". ");
    }
    stream.print(value1, value2, value3, value4, value5, ".");
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType> template <typename A, typename B, typename C, typename D, typename E, typename F>
void Parser<LexerType>::logError(bool shouldPrintToken, const A& value1, const B& value2, const C& value3, const D& value4, const E& value5, const F& value6)
{
    if (hasError())
        return;
    StringPrintStream stream;
    if (shouldPrintToken) {
        printUnexpectedTokenText(stream);
        stream.print(". ");
    }
    stream.print(value1, value2, value3, value4, value5, value6, ".");
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType> template <typename A, typename B, typename C, typename D, typename E, typename F, typename G>
void Parser<LexerType>::logError(bool shouldPrintToken, const A& value1, const B& value2, const C& value3, const D& value4, const E& value5, const F& value6, const G& value7)
{
    if (hasError())
        return;
    StringPrintStream stream;
    if (shouldPrintToken) {
        printUnexpectedTokenText(stream);
        stream.print(". ");
    }
    stream.print(value1, value2, value3, value4, value5, value6, value7, ".");
    setErrorMessage(stream.toStringWithLatin1Fallback());
}

template <typename LexerType>
Parser<LexerType>::Parser(
    VM* vm, const SourceCode& source, JSParserBuiltinMode builtinMode, 
    JSParserStrictMode strictMode, SourceParseMode parseMode, SuperBinding superBinding,
    ConstructorKind defaultConstructorKind, ThisTDZMode thisTDZMode)
    : m_vm(vm)
    , m_source(&source)
    , m_hasStackOverflow(false)
    , m_allowsIn(true)
    , m_syntaxAlreadyValidated(source.provider()->isValid())
    , m_statementDepth(0)
    , m_sourceElements(0)
    , m_parsingBuiltin(builtinMode == JSParserBuiltinMode::Builtin)
    , m_superBinding(superBinding)
    , m_defaultConstructorKind(defaultConstructorKind)
    , m_thisTDZMode(thisTDZMode)
{
    m_lexer = std::make_unique<LexerType>(vm, builtinMode);
    m_lexer->setCode(source, &m_parserArena);
    m_token.m_location.line = source.firstLine();
    m_token.m_location.startOffset = source.startOffset();
    m_token.m_location.endOffset = source.startOffset();
    m_token.m_location.lineStartOffset = source.startOffset();
    m_functionCache = vm->addSourceProviderCache(source.provider());
    m_expressionErrorClassifier = nullptr;

    ScopeRef scope = pushScope();
    scope->setSourceParseMode(parseMode);

    if (strictMode == JSParserStrictMode::Strict)
        scope->setStrictMode();

    next();
}

template <typename LexerType>
Parser<LexerType>::~Parser()
{
}

template <typename LexerType>
String Parser<LexerType>::parseInner(const Identifier& calleeName, SourceParseMode parseMode)
{
    String parseError = String();

    ASTBuilder context(const_cast<VM*>(m_vm), m_parserArena, const_cast<SourceCode*>(m_source));
    ScopeRef scope = currentScope();
    scope->setIsLexicalScope();
    SetForScope<FunctionParsePhase> functionParsePhasePoisoner(m_parserState.functionParsePhase, FunctionParsePhase::Body);

    bool isArrowFunctionBodyExpression = false;
    if (m_lexer->isReparsingFunction()) {
        ParserFunctionInfo<ASTBuilder> functionInfo;
        if (parseMode == SourceParseMode::GeneratorBodyMode)
            functionInfo.parameters = createGeneratorParameters(context);
        else
            parseFunctionParameters(context, parseMode, functionInfo);
        m_parameters = functionInfo.parameters;

        if (parseMode == SourceParseMode::ArrowFunctionMode && !hasError()) {
            // The only way we could have an error wile reparsing is if we run out of stack space.
            RELEASE_ASSERT(match(ARROWFUNCTION));
            next();
            isArrowFunctionBodyExpression = !match(OPENBRACE);
        }
    }

    if (!calleeName.isNull())
        scope->declareCallee(&calleeName);

    if (m_lexer->isReparsingFunction())
        m_statementDepth--;

    SourceElements* sourceElements = nullptr;
    // The only way we can error this early is if we reparse a function and we run out of stack space.
    if (!hasError()) {
        if (isArrowFunctionBodyExpression)
            sourceElements = parseArrowFunctionSingleExpressionBodySourceElements(context);
        else if (isModuleParseMode(parseMode))
            sourceElements = parseModuleSourceElements(context, parseMode);
        else {
            if (parseMode == SourceParseMode::GeneratorWrapperFunctionMode)
                sourceElements = parseGeneratorFunctionSourceElements(context, CheckForStrictMode);
            else
                sourceElements = parseSourceElements(context, CheckForStrictMode);
        }
    }

    bool validEnding;
    if (isArrowFunctionBodyExpression) {
        ASSERT(m_lexer->isReparsingFunction());
        // When we reparse and stack overflow, we're not guaranteed a valid ending. If we don't run out of stack space,
        // then of course this will always be valid because we already parsed for syntax errors. But we must
        // be cautious in case we run out of stack space.
        validEnding = isEndOfArrowFunction(); 
    } else
        validEnding = consume(EOFTOK);

    if (!sourceElements || !validEnding) {
        if (hasError())
            parseError = m_errorMessage;
        else
            parseError = ASCIILiteral("Parser error");
    }

    IdentifierSet capturedVariables;
    bool modifiedParameter = false;
    bool modifiedArguments = false;
    scope->getCapturedVars(capturedVariables, modifiedParameter, modifiedArguments);

    VariableEnvironment& varDeclarations = scope->declaredVariables();
    for (auto& entry : capturedVariables)
        varDeclarations.markVariableAsCaptured(entry);

    IdentifierSet usedVariables;
    scope->getUsedVariables(usedVariables);
    if (parseMode == SourceParseMode::GeneratorWrapperFunctionMode) {
        if (usedVariables.contains(m_vm->propertyNames->arguments.impl()))
            context.propagateArgumentsUse();
    }

    CodeFeatures features = context.features();
    if (scope->strictMode())
        features |= StrictModeFeature;
    if (scope->shadowsArguments())
        features |= ShadowsArgumentsFeature;
    if (modifiedParameter)
        features |= ModifiedParameterFeature;
    if (modifiedArguments)
        features |= ModifiedArgumentsFeature;

#ifndef NDEBUG
    if (m_parsingBuiltin && isProgramParseMode(parseMode)) {
        VariableEnvironment& lexicalVariables = scope->lexicalVariables();
        const IdentifierSet& closedVariableCandidates = scope->closedVariableCandidates();
        const BuiltinNames& builtinNames = m_vm->propertyNames->builtinNames();
        for (const RefPtr<UniquedStringImpl>& candidate : closedVariableCandidates) {
            if (!lexicalVariables.contains(candidate) && !varDeclarations.contains(candidate) && !builtinNames.isPrivateName(*candidate.get())) {
                dataLog("Bad global capture in builtin: '", candidate, "'\n");
                dataLog(m_source->view());
                CRASH();
            }
        }
    }
#endif // NDEBUG
    didFinishParsing(sourceElements, context.funcDeclarations(), varDeclarations, features, context.numConstants());

    return parseError;
}

template <typename LexerType>
void Parser<LexerType>::didFinishParsing(SourceElements* sourceElements, DeclarationStacks::FunctionStack& funcStack, 
    VariableEnvironment& varDeclarations, CodeFeatures features, int numConstants)
{
    m_sourceElements = sourceElements;
    m_funcDeclarations.swap(funcStack);
    m_varDeclarations.swap(varDeclarations);
    m_features = features;
    m_numConstants = numConstants;
}

template <typename LexerType>
bool Parser<LexerType>::isArrowFunctionParameters()
{
    bool isArrowFunction = false;

    if (match(EOFTOK))
        return false;
    
    bool isOpenParen = match(OPENPAREN);
    bool isIdent = match(IDENT);
    
    if (!isOpenParen && !isIdent)
        return false;

    SavePoint saveArrowFunctionPoint = createSavePoint();
        
    if (isIdent) {
        next();
        isArrowFunction = match(ARROWFUNCTION);
    } else {
        RELEASE_ASSERT(isOpenParen);
        next();
        if (match(CLOSEPAREN)) {
            next();
            isArrowFunction = match(ARROWFUNCTION);
        } else {
            SyntaxChecker syntaxChecker(const_cast<VM*>(m_vm), m_lexer.get());
            // We make fake scope, otherwise parseFormalParameters will add variable to current scope that lead to errors
            AutoPopScopeRef fakeScope(this, pushScope());
            fakeScope->setSourceParseMode(SourceParseMode::ArrowFunctionMode);

            unsigned parametersCount = 0;
            isArrowFunction = parseFormalParameters(syntaxChecker, syntaxChecker.createFormalParameterList(), parametersCount) && consume(CLOSEPAREN) && match(ARROWFUNCTION);
                
            popScope(fakeScope, syntaxChecker.NeedsFreeVariableInfo);
        }
    }
        
    restoreSavePoint(saveArrowFunctionPoint);
        
    return isArrowFunction;
}

template <typename LexerType>
bool Parser<LexerType>::allowAutomaticSemicolon()
{
    return match(CLOSEBRACE) || match(EOFTOK) || m_lexer->prevTerminator();
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseSourceElements(TreeBuilder& context, SourceElementsMode mode)
{
    const unsigned lengthOfUseStrictLiteral = 12; // "use strict".length
    TreeSourceElements sourceElements = context.createSourceElements();
    bool seenNonDirective = false;
    const Identifier* directive = 0;
    unsigned directiveLiteralLength = 0;
    auto savePoint = createSavePoint();
    bool hasSetStrict = false;
    
    while (TreeStatement statement = parseStatementListItem(context, directive, &directiveLiteralLength)) {
        if (mode == CheckForStrictMode && !seenNonDirective) {
            if (directive) {
                // "use strict" must be the exact literal without escape sequences or line continuation.
                if (!hasSetStrict && directiveLiteralLength == lengthOfUseStrictLiteral && m_vm->propertyNames->useStrictIdentifier == *directive) {
                    setStrictMode();
                    hasSetStrict = true;
                    if (!isValidStrictMode()) {
                        if (m_parserState.lastFunctionName) {
                            if (m_vm->propertyNames->arguments == *m_parserState.lastFunctionName)
                                semanticFail("Cannot name a function 'arguments' in strict mode");
                            if (m_vm->propertyNames->eval == *m_parserState.lastFunctionName)
                                semanticFail("Cannot name a function 'eval' in strict mode");
                        }
                        if (hasDeclaredVariable(m_vm->propertyNames->arguments))
                            semanticFail("Cannot declare a variable named 'arguments' in strict mode");
                        if (hasDeclaredVariable(m_vm->propertyNames->eval))
                            semanticFail("Cannot declare a variable named 'eval' in strict mode");
                        semanticFailIfFalse(isValidStrictMode(), "Invalid parameters or function name in strict mode");
                    }
                    restoreSavePoint(savePoint);
                    propagateError();
                    continue;
                }
            } else
                seenNonDirective = true;
        }
        context.appendStatement(sourceElements, statement);
    }

    propagateError();
    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseModuleSourceElements(TreeBuilder& context, SourceParseMode parseMode)
{
    TreeSourceElements sourceElements = context.createSourceElements();
    SyntaxChecker syntaxChecker(const_cast<VM*>(m_vm), m_lexer.get());

    while (true) {
        TreeStatement statement = 0;
        if (match(IMPORT))
            statement = parseImportDeclaration(context);
        else if (match(EXPORT))
            statement = parseExportDeclaration(context);
        else {
            const Identifier* directive = 0;
            unsigned directiveLiteralLength = 0;
            if (parseMode == SourceParseMode::ModuleAnalyzeMode) {
                if (!parseStatementListItem(syntaxChecker, directive, &directiveLiteralLength))
                    break;
                continue;
            }
            statement = parseStatementListItem(context, directive, &directiveLiteralLength);
        }

        if (!statement)
            break;
        context.appendStatement(sourceElements, statement);
    }

    propagateError();

    for (const auto& uid : currentScope()->moduleScopeData().exportedBindings()) {
        if (currentScope()->hasDeclaredVariable(uid)) {
            currentScope()->declaredVariables().markVariableAsExported(uid);
            continue;
        }

        if (currentScope()->hasLexicallyDeclaredVariable(uid)) {
            currentScope()->lexicalVariables().markVariableAsExported(uid);
            continue;
        }

        semanticFail("Exported binding '", uid.get(), "' needs to refer to a top-level declared variable");
    }

    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseGeneratorFunctionSourceElements(TreeBuilder& context, SourceElementsMode mode)
{
    auto sourceElements = context.createSourceElements();

    unsigned functionKeywordStart = tokenStart();
    JSTokenLocation startLocation(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    unsigned startColumn = tokenColumn();
    int functionNameStart = m_token.m_location.startOffset;
    int parametersStart = m_token.m_location.startOffset;

    ParserFunctionInfo<TreeBuilder> info;
    info.name = &m_vm->propertyNames->nullIdentifier;
    info.parameters = createGeneratorParameters(context);
    info.startOffset = parametersStart;
    info.startLine = tokenLine();
    info.parameterCount = 4; // generator, state, value, resume mode

    {
        AutoPopScopeRef generatorBodyScope(this, pushScope());
        generatorBodyScope->setSourceParseMode(SourceParseMode::GeneratorBodyMode);
        SyntaxChecker generatorFunctionContext(const_cast<VM*>(m_vm), m_lexer.get());
        failIfFalse(parseSourceElements(generatorFunctionContext, mode), "Cannot parse the body of a generator");
        popScope(generatorBodyScope, TreeBuilder::NeedsFreeVariableInfo);
    }
    info.body = context.createFunctionMetadata(startLocation, tokenLocation(), startColumn, tokenColumn(), functionKeywordStart, functionNameStart, parametersStart, strictMode(), ConstructorKind::None, m_superBinding, info.parameterCount, SourceParseMode::GeneratorBodyMode, false);

    info.endLine = tokenLine();
    info.endOffset = m_token.m_data.offset;
    info.bodyStartColumn = startColumn;

    auto functionExpr = context.createFunctionExpr(startLocation, info);
    auto statement = context.createExprStatement(startLocation, functionExpr, start, m_lastTokenEndPosition.line);
    context.appendStatement(sourceElements, statement);

    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseStatementListItem(TreeBuilder& context, const Identifier*& directive, unsigned* directiveLiteralLength)
{
    // The grammar is documented here:
    // http://www.ecma-international.org/ecma-262/6.0/index.html#sec-statements
    DepthManager statementDepth(&m_statementDepth);
    m_statementDepth++;
    TreeStatement result = 0;
    bool shouldSetEndOffset = true;
    switch (m_token.m_type) {
    case CONSTTOKEN:
        result = parseVariableDeclaration(context, DeclarationType::ConstDeclaration);
        break;
    case LET: {
        bool shouldParseVariableDeclaration = true;
        if (!strictMode()) {
            SavePoint savePoint = createSavePoint();
            next();
            // Intentionally use `match(IDENT) || match(LET) || match(YIELD)` and don't use `matchSpecIdentifier()`.
            // We would like to fall into parseVariableDeclaration path even if "yield" is not treated as an Identifier.
            // For example, under a generator context, matchSpecIdentifier() for "yield" returns `false`.
            // But we would like to enter parseVariableDeclaration and raise an error under the context of parseVariableDeclaration
            // to raise consistent errors between "var", "const" and "let".
            if (!(match(IDENT) || match(LET) || match(YIELD)) && !match(OPENBRACE) && !match(OPENBRACKET))
                shouldParseVariableDeclaration = false;
            restoreSavePoint(savePoint);
        }
        if (shouldParseVariableDeclaration)
            result = parseVariableDeclaration(context, DeclarationType::LetDeclaration);
        else
            result = parseExpressionOrLabelStatement(context); // Treat this as an IDENT. This is how ::parseStatement() handles IDENT.

        break;
    }
#if ENABLE(ES6_CLASS_SYNTAX)
    case CLASSTOKEN:
        result = parseClassDeclaration(context);
        break;
#endif
    default:
        m_statementDepth--; // parseStatement() increments the depth.
        result = parseStatement(context, directive, directiveLiteralLength);
        shouldSetEndOffset = false;
        break;
    }

    if (result && shouldSetEndOffset)
        context.setEndOffset(result, m_lastTokenEndPosition.offset);

    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseVariableDeclaration(TreeBuilder& context, DeclarationType declarationType, ExportType exportType)
{
    ASSERT(match(VAR) || match(LET) || match(CONSTTOKEN));
    JSTokenLocation location(tokenLocation());
    int start = tokenLine();
    int end = 0;
    int scratch;
    TreeDestructuringPattern scratch1 = 0;
    TreeExpression scratch2 = 0;
    JSTextPosition scratch3;
    bool scratchBool;
    TreeExpression variableDecls = parseVariableDeclarationList(context, scratch, scratch1, scratch2, scratch3, scratch3, scratch3, VarDeclarationContext, declarationType, exportType, scratchBool);
    propagateError();
    failIfFalse(autoSemiColon(), "Expected ';' after variable declaration");
    
    return context.createDeclarationStatement(location, variableDecls, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseDoWhileStatement(TreeBuilder& context)
{
    ASSERT(match(DO));
    int startLine = tokenLine();
    next();
    const Identifier* unused = 0;
    startLoop();
    TreeStatement statement = parseStatement(context, unused);
    endLoop();
    failIfFalse(statement, "Expected a statement following 'do'");
    int endLine = tokenLine();
    JSTokenLocation location(tokenLocation());
    handleProductionOrFail(WHILE, "while", "end", "do-while loop");
    handleProductionOrFail(OPENPAREN, "(", "start", "do-while loop condition");
    semanticFailIfTrue(match(CLOSEPAREN), "Must provide an expression as a do-while loop condition");
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Unable to parse do-while loop condition");
    handleProductionOrFail(CLOSEPAREN, ")", "end", "do-while loop condition");
    if (match(SEMICOLON))
        next(); // Always performs automatic semicolon insertion.
    return context.createDoWhileStatement(location, statement, expr, startLine, endLine);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseWhileStatement(TreeBuilder& context)
{
    ASSERT(match(WHILE));
    JSTokenLocation location(tokenLocation());
    int startLine = tokenLine();
    next();
    
    handleProductionOrFail(OPENPAREN, "(", "start", "while loop condition");
    semanticFailIfTrue(match(CLOSEPAREN), "Must provide an expression as a while loop condition");
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Unable to parse while loop condition");
    int endLine = tokenLine();
    handleProductionOrFail(CLOSEPAREN, ")", "end", "while loop condition");
    
    const Identifier* unused = 0;
    startLoop();
    TreeStatement statement = parseStatement(context, unused);
    endLoop();
    failIfFalse(statement, "Expected a statement as the body of a while loop");
    return context.createWhileStatement(location, expr, statement, startLine, endLine);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseVariableDeclarationList(TreeBuilder& context, int& declarations, TreeDestructuringPattern& lastPattern, TreeExpression& lastInitializer, JSTextPosition& identStart, JSTextPosition& initStart, JSTextPosition& initEnd, VarDeclarationListContext declarationListContext, DeclarationType declarationType, ExportType exportType, bool& forLoopConstDoesNotHaveInitializer)
{
    ASSERT(declarationType == DeclarationType::LetDeclaration || declarationType == DeclarationType::VarDeclaration || declarationType == DeclarationType::ConstDeclaration);
    TreeExpression head = 0;
    TreeExpression tail = 0;
    const Identifier* lastIdent;
    JSToken lastIdentToken; 
    AssignmentContext assignmentContext = assignmentContextFromDeclarationType(declarationType);
    do {
        lastIdent = 0;
        lastPattern = TreeDestructuringPattern(0);
        JSTokenLocation location(tokenLocation());
        next();
        TreeExpression node = 0;
        declarations++;
        bool hasInitializer = false;
        if (matchSpecIdentifier()) {
            failIfTrue(match(LET) && (declarationType == DeclarationType::LetDeclaration || declarationType == DeclarationType::ConstDeclaration), 
                "Can't use 'let' as an identifier name for a LexicalDeclaration");
            JSTextPosition varStart = tokenStartPosition();
            JSTokenLocation varStartLocation(tokenLocation());
            identStart = varStart;
            const Identifier* name = m_token.m_data.ident;
            lastIdent = name;
            lastIdentToken = m_token;
            next();
            hasInitializer = match(EQUAL);
            DeclarationResultMask declarationResult = declareVariable(name, declarationType);
            if (declarationResult != DeclarationResult::Valid) {
                failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot declare a variable named ", name->impl(), " in strict mode");
                if (declarationResult & DeclarationResult::InvalidDuplicateDeclaration) {
                    if (declarationType == DeclarationType::LetDeclaration) 
                        internalFailWithMessage(false, "Cannot declare a let variable twice: '", name->impl(), "'");
                    if (declarationType == DeclarationType::ConstDeclaration)
                        internalFailWithMessage(false, "Cannot declare a const variable twice: '", name->impl(), "'");
                    ASSERT(declarationType == DeclarationType::VarDeclaration);
                    internalFailWithMessage(false, "Cannot declare a var variable that shadows a let/const/class variable: '", name->impl(), "'");
                }
            }
            if (exportType == ExportType::Exported) {
                semanticFailIfFalse(exportName(*name), "Cannot export a duplicate name '", name->impl(), "'");
                currentScope()->moduleScopeData().exportBinding(*name);
            }

            if (hasInitializer) {
                JSTextPosition varDivot = tokenStartPosition() + 1;
                initStart = tokenStartPosition();
                next(TreeBuilder::DontBuildStrings); // consume '='
                propagateError();
                TreeExpression initializer = parseAssignmentExpression(context);
                initEnd = lastTokenEndPosition();
                lastInitializer = initializer;
                failIfFalse(initializer, "Expected expression as the intializer for the variable '", name->impl(), "'");
                
                node = context.createAssignResolve(location, *name, initializer, varStart, varDivot, lastTokenEndPosition(), assignmentContext);
            } else {
                if (declarationListContext == ForLoopContext && declarationType == DeclarationType::ConstDeclaration)
                    forLoopConstDoesNotHaveInitializer = true;
                failIfTrue(declarationListContext != ForLoopContext && declarationType == DeclarationType::ConstDeclaration, "const declared variable '", name->impl(), "'", " must have an initializer");
                if (declarationType == DeclarationType::VarDeclaration)
                    node = context.createEmptyVarExpression(varStartLocation, *name);
                else
                    node = context.createEmptyLetExpression(varStartLocation, *name);
            }
        } else {
            lastIdent = 0;
            auto pattern = parseDestructuringPattern(context, destructuringKindFromDeclarationType(declarationType), exportType, nullptr, nullptr, assignmentContext);
            failIfFalse(pattern, "Cannot parse this destructuring pattern");
            hasInitializer = match(EQUAL);
            failIfTrue(declarationListContext == VarDeclarationContext && !hasInitializer, "Expected an initializer in destructuring variable declaration");
            lastPattern = pattern;
            if (hasInitializer) {
                next(TreeBuilder::DontBuildStrings); // consume '='
                TreeExpression rhs = parseAssignmentExpression(context);
                node = context.createDestructuringAssignment(location, pattern, rhs);
                lastInitializer = rhs;
            }
        }

        if (node) {
            if (!head)
                head = node;
            else if (!tail) {
                head = context.createCommaExpr(location, head);
                tail = context.appendToCommaExpr(location, head, head, node);
            } else
                tail = context.appendToCommaExpr(location, head, tail, node);
        }
    } while (match(COMMA));
    if (lastIdent)
        lastPattern = context.createBindingLocation(lastIdentToken.m_location, *lastIdent, lastIdentToken.m_startPosition, lastIdentToken.m_endPosition, assignmentContext);

    return head;
}

template <typename LexerType>
bool Parser<LexerType>::declareRestOrNormalParameter(const Identifier& name, const Identifier** duplicateIdentifier)
{
    DeclarationResultMask declarationResult = declareParameter(&name);
    if ((declarationResult & DeclarationResult::InvalidStrictMode) && strictMode()) {
        semanticFailIfTrue(isEvalOrArguments(&name), "Cannot destructure to a parameter name '", name.impl(), "' in strict mode");
        if (m_parserState.lastFunctionName && name == *m_parserState.lastFunctionName)
            semanticFail("Cannot declare a parameter named '", name.impl(), "' as it shadows the name of a strict mode function");
        semanticFailureDueToKeyword("parameter name");
        if (hasDeclaredParameter(name))
            semanticFail("Cannot declare a parameter named '", name.impl(), "' in strict mode as it has already been declared");
        semanticFail("Cannot declare a parameter named '", name.impl(), "' in strict mode");
    }
    if (declarationResult & DeclarationResult::InvalidDuplicateDeclaration) {
        // It's not always an error to define a duplicate parameter.
        // It's only an error when there are default parameter values or destructuring parameters.
        // We note this value now so we can check it later.
        if (duplicateIdentifier)
            *duplicateIdentifier = &name;
    }

    return true;
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::createBindingPattern(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier& name, JSToken token, AssignmentContext bindingContext, const Identifier** duplicateIdentifier)
{
    ASSERT(!name.isNull());
    
    ASSERT(name.impl()->isAtomic() || name.impl()->isSymbol());

    switch (kind) {
    case DestructuringKind::DestructureToVariables: {
        DeclarationResultMask declarationResult = declareVariable(&name);
        failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot declare a variable named '", name.impl(), "' in strict mode");
        if (declarationResult & DeclarationResult::InvalidDuplicateDeclaration)
            internalFailWithMessage(false, "Cannot declare a var variable that shadows a let/const/class variable: '", name.impl(), "'");
        break;
    }

    case DestructuringKind::DestructureToLet:
    case DestructuringKind::DestructureToConst:
    case DestructuringKind::DestructureToCatchParameters: {
        DeclarationResultMask declarationResult = declareVariable(&name, kind == DestructuringKind::DestructureToConst ? DeclarationType::ConstDeclaration : DeclarationType::LetDeclaration);
        if (declarationResult != DeclarationResult::Valid) {
            failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot destructure to a variable named '", name.impl(), "' in strict mode");
            failIfTrue(declarationResult & DeclarationResult::InvalidDuplicateDeclaration, "Cannot declare a lexical variable twice: '", name.impl(), "'");
        }
        break;
    }

    case DestructuringKind::DestructureToParameters: {
        declareRestOrNormalParameter(name, duplicateIdentifier);
        propagateError();
        break;
    }

    case DestructuringKind::DestructureToExpressions: {
        break;
    }
    }

    if (exportType == ExportType::Exported) {
        semanticFailIfFalse(exportName(name), "Cannot export a duplicate name '", name.impl(), "'");
        currentScope()->moduleScopeData().exportBinding(name);
    }
    return context.createBindingLocation(token.m_location, name, token.m_startPosition, token.m_endPosition, bindingContext);
}

template <typename LexerType>
template <class TreeBuilder> NEVER_INLINE TreeDestructuringPattern Parser<LexerType>::createAssignmentElement(TreeBuilder& context, TreeExpression& assignmentTarget, const JSTextPosition& startPosition, const JSTextPosition& endPosition)
{
    return context.createAssignmentElement(assignmentTarget, startPosition, endPosition);
}

template <typename LexerType>
template <class TreeBuilder> TreeSourceElements Parser<LexerType>::parseArrowFunctionSingleExpressionBodySourceElements(TreeBuilder& context)
{
    ASSERT(!match(OPENBRACE));

    JSTokenLocation location(tokenLocation());
    JSTextPosition start = tokenStartPosition();

    failIfStackOverflow();
    TreeExpression expr = parseAssignmentExpression(context);
    failIfFalse(expr, "Cannot parse the arrow function expression");
    
    context.setEndOffset(expr, m_lastTokenEndPosition.offset);

    failIfFalse(isEndOfArrowFunction(), "Expected a ';', ']', '}', ')', ',', line terminator or EOF following a arrow function statement");

    JSTextPosition end = tokenEndPosition();
    
    if (!m_lexer->prevTerminator())
        setEndOfStatement();

    TreeSourceElements sourceElements = context.createSourceElements();
    TreeStatement body = context.createReturnStatement(location, expr, start, end);
    context.setEndOffset(body, m_lastTokenEndPosition.offset);
    context.appendStatement(sourceElements, body);

    return sourceElements;
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::tryParseDestructuringPatternExpression(TreeBuilder& context, AssignmentContext bindingContext)
{
    return parseDestructuringPattern(context, DestructuringKind::DestructureToExpressions, ExportType::NotExported, nullptr, nullptr, bindingContext);
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::parseBindingOrAssignmentElement(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier** duplicateIdentifier, bool* hasDestructuringPattern, AssignmentContext bindingContext, int depth)
{
    if (kind == DestructuringKind::DestructureToExpressions)
        return parseAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth);
    return parseDestructuringPattern(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth);
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::parseAssignmentElement(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier** duplicateIdentifier, bool* hasDestructuringPattern, AssignmentContext bindingContext, int depth)
{
    TreeDestructuringPattern assignmentTarget = 0;

    if (match(OPENBRACE) || match(OPENBRACKET)) {
        SavePoint savePoint = createSavePoint();
        assignmentTarget = parseDestructuringPattern(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth);
        if (assignmentTarget && !match(DOT) && !match(OPENBRACKET) && !match(OPENPAREN) && !match(TEMPLATE))
            return assignmentTarget;
        restoreSavePoint(savePoint);
    }

    JSTextPosition startPosition = tokenStartPosition();
    auto element = parseMemberExpression(context);

    semanticFailIfFalse(element && context.isAssignmentLocation(element), "Invalid destructuring assignment target");

    if (strictMode() && m_parserState.lastIdentifier && context.isResolve(element)) {
        bool isEvalOrArguments = m_vm->propertyNames->eval == *m_parserState.lastIdentifier || m_vm->propertyNames->arguments == *m_parserState.lastIdentifier;
        failIfTrueIfStrict(isEvalOrArguments, "Cannot modify '", m_parserState.lastIdentifier->impl(), "' in strict mode");
    }

    return createAssignmentElement(context, element, startPosition, lastTokenEndPosition());
}

static const char* destructuringKindToVariableKindName(DestructuringKind kind)
{
    switch (kind) {
    case DestructuringKind::DestructureToLet:
    case DestructuringKind::DestructureToConst:
        return "lexical variable name";
    case DestructuringKind::DestructureToVariables:
        return "variable name";
    case DestructuringKind::DestructureToParameters:
        return "parameter name";
    case DestructuringKind::DestructureToCatchParameters:
        return "catch parameter name";
    case DestructuringKind::DestructureToExpressions:
        return "expression name";
    }
    RELEASE_ASSERT_NOT_REACHED();
    return "invalid";
}

template <typename LexerType>
template <class TreeBuilder> TreeDestructuringPattern Parser<LexerType>::parseDestructuringPattern(TreeBuilder& context, DestructuringKind kind, ExportType exportType, const Identifier** duplicateIdentifier, bool* hasDestructuringPattern, AssignmentContext bindingContext, int depth)
{
    failIfStackOverflow();
    int nonLHSCount = m_parserState.nonLHSCount;
    TreeDestructuringPattern pattern;
    switch (m_token.m_type) {
    case OPENBRACKET: {
        JSTextPosition divotStart = tokenStartPosition();
        auto arrayPattern = context.createArrayPattern(m_token.m_location);
        next();

        if (hasDestructuringPattern)
            *hasDestructuringPattern = true;

        bool restElementWasFound = false;

        do {
            while (match(COMMA)) {
                context.appendArrayPatternSkipEntry(arrayPattern, m_token.m_location);
                next();
            }
            propagateError();

            if (match(CLOSEBRACKET))
                break;

            if (UNLIKELY(match(DOTDOTDOT))) {
                JSTokenLocation location = m_token.m_location;
                next();
                auto innerPattern = parseBindingOrAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth + 1);
                if (kind == DestructuringKind::DestructureToExpressions && !innerPattern)
                    return 0;
                failIfFalse(innerPattern, "Cannot parse this destructuring pattern");

                failIfTrue(kind != DestructuringKind::DestructureToExpressions && !context.isBindingNode(innerPattern),  "Expected identifier for a rest element destructuring pattern");

                context.appendArrayPatternRestEntry(arrayPattern, location, innerPattern);
                restElementWasFound = true;
                break;
            }

            JSTokenLocation location = m_token.m_location;
            auto innerPattern = parseBindingOrAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth + 1);
            if (kind == DestructuringKind::DestructureToExpressions && !innerPattern)
                return 0;
            failIfFalse(innerPattern, "Cannot parse this destructuring pattern");
            TreeExpression defaultValue = parseDefaultValueForDestructuringPattern(context);
            context.appendArrayPatternEntry(arrayPattern, location, innerPattern, defaultValue);
        } while (consume(COMMA));

        consumeOrFail(CLOSEBRACKET, restElementWasFound ? "Expected a closing ']' following a rest element destructuring pattern" : "Expected either a closing ']' or a ',' following an element destructuring pattern");
        context.finishArrayPattern(arrayPattern, divotStart, divotStart, lastTokenEndPosition());
        pattern = arrayPattern;
        break;
    }
    case OPENBRACE: {
        auto objectPattern = context.createObjectPattern(m_token.m_location);
        next();

        if (hasDestructuringPattern)
            *hasDestructuringPattern = true;

        do {
            bool wasString = false;

            if (match(CLOSEBRACE))
                break;

            const Identifier* propertyName = nullptr;
            TreeExpression propertyExpression = 0;
            TreeDestructuringPattern innerPattern = 0;
            JSTokenLocation location = m_token.m_location;
            if (matchSpecIdentifier()) {
                failIfTrue(match(LET) && (kind == DestructuringKind::DestructureToLet || kind == DestructuringKind::DestructureToConst), "Can't use 'let' as an identifier name for a LexicalDeclaration");
                propertyName = m_token.m_data.ident;
                JSToken identifierToken = m_token;
                next();
                if (consume(COLON))
                    innerPattern = parseBindingOrAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth + 1);
                else {
                    if (kind == DestructuringKind::DestructureToExpressions) {
                        bool isEvalOrArguments = m_vm->propertyNames->eval == *propertyName || m_vm->propertyNames->arguments == *propertyName;
                        if (isEvalOrArguments && strictMode())
                            reclassifyExpressionError(ErrorIndicatesPattern, ErrorIndicatesNothing);
                        failIfTrueIfStrict(isEvalOrArguments, "Cannot modify '", propertyName->impl(), "' in strict mode");
                    }
                    innerPattern = createBindingPattern(context, kind, exportType, *propertyName, identifierToken, bindingContext, duplicateIdentifier);
                }
            } else {
                JSTokenType tokenType = m_token.m_type;
                switch (m_token.m_type) {
                case DOUBLE:
                case INTEGER:
                    propertyName = &m_parserArena.identifierArena().makeNumericIdentifier(const_cast<VM*>(m_vm), m_token.m_data.doubleValue);
                    break;
                case STRING:
                    propertyName = m_token.m_data.ident;
                    wasString = true;
                    break;
                case OPENBRACKET:
                    next();
                    propertyExpression = parseAssignmentExpression(context);
                    failIfFalse(propertyExpression, "Cannot parse computed property name");
                    matchOrFail(CLOSEBRACKET, "Expected ']' to end end a computed property name");
                    break;
                default:
                    if (m_token.m_type != RESERVED && m_token.m_type != RESERVED_IF_STRICT && !(m_token.m_type & KeywordTokenFlag)) {
                        if (kind == DestructuringKind::DestructureToExpressions)
                            return 0;
                        failWithMessage("Expected a property name");
                    }
                    propertyName = m_token.m_data.ident;
                    break;
                }
                next();
                if (!consume(COLON)) {
                    if (kind == DestructuringKind::DestructureToExpressions)
                        return 0;
                    semanticFailIfTrue(tokenType == RESERVED, "Cannot use abbreviated destructuring syntax for reserved name '", propertyName->impl(), "'");
                    semanticFailIfTrue(tokenType == RESERVED_IF_STRICT, "Cannot use abbreviated destructuring syntax for reserved name '", propertyName->impl(), "' in strict mode");
                    semanticFailIfTrue(tokenType & KeywordTokenFlag, "Cannot use abbreviated destructuring syntax for keyword '", propertyName->impl(), "'");
                    
                    failWithMessage("Expected a ':' prior to a named destructuring property");
                }
                innerPattern = parseBindingOrAssignmentElement(context, kind, exportType, duplicateIdentifier, hasDestructuringPattern, bindingContext, depth + 1);
            }
            if (kind == DestructuringKind::DestructureToExpressions && !innerPattern)
                return 0;
            failIfFalse(innerPattern, "Cannot parse this destructuring pattern");
            TreeExpression defaultValue = parseDefaultValueForDestructuringPattern(context);
            if (propertyExpression)
                context.appendObjectPatternEntry(objectPattern, location, propertyExpression, innerPattern, defaultValue);
            else {
                ASSERT(propertyName);
                context.appendObjectPatternEntry(objectPattern, location, wasString, *propertyName, innerPattern, defaultValue);
            }
        } while (consume(COMMA));

        if (kind == DestructuringKind::DestructureToExpressions && !match(CLOSEBRACE))
            return 0;
        consumeOrFail(CLOSEBRACE, "Expected either a closing '}' or an ',' after a property destructuring pattern");
        pattern = objectPattern;
        break;
    }

    default: {
        if (!matchSpecIdentifier()) {
            if (kind == DestructuringKind::DestructureToExpressions)
                return 0;
            semanticFailureDueToKeyword(destructuringKindToVariableKindName(kind));
            failWithMessage("Expected a parameter pattern or a ')' in parameter list");
        }
        failIfTrue(match(LET) && (kind == DestructuringKind::DestructureToLet || kind == DestructuringKind::DestructureToConst), "Can't use 'let' as an identifier name for a LexicalDeclaration");
        pattern = createBindingPattern(context, kind, exportType, *m_token.m_data.ident, m_token, bindingContext, duplicateIdentifier);
        next();
        break;
    }
    }
    m_parserState.nonLHSCount = nonLHSCount;
    return pattern;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseDefaultValueForDestructuringPattern(TreeBuilder& context)
{
    if (!match(EQUAL))
        return 0;

    next(TreeBuilder::DontBuildStrings); // consume '='
    return parseAssignmentExpression(context);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseForStatement(TreeBuilder& context)
{
    ASSERT(match(FOR));
    JSTokenLocation location(tokenLocation());
    int startLine = tokenLine();
    next();

    DepthManager statementDepth(&m_statementDepth);
    m_statementDepth++;

    handleProductionOrFail(OPENPAREN, "(", "start", "for-loop header");
    int nonLHSCount = m_parserState.nonLHSCount;
    int declarations = 0;
    JSTextPosition declsStart;
    JSTextPosition declsEnd;
    TreeExpression decls = 0;
    TreeDestructuringPattern pattern = 0;
    bool isVarDeclaraton = match(VAR);
    bool isLetDeclaration = match(LET);
    bool isConstDeclaration = match(CONSTTOKEN);
    bool forLoopConstDoesNotHaveInitializer = false;

    VariableEnvironment dummySet;
    VariableEnvironment* lexicalVariables = nullptr;
    AutoCleanupLexicalScope lexicalScope;

    auto gatherLexicalVariablesIfNecessary = [&] {
        if (isLetDeclaration || isConstDeclaration) {
            ScopeRef scope = lexicalScope.scope();
            lexicalVariables = &scope->finalizeLexicalEnvironment();
        } else
            lexicalVariables = &dummySet;
    };

    auto popLexicalScopeIfNecessary = [&] {
        if (isLetDeclaration || isConstDeclaration)
            popScope(lexicalScope, TreeBuilder::NeedsFreeVariableInfo);
    };

    if (isVarDeclaraton || isLetDeclaration || isConstDeclaration) {
        /*
         for (var/let/const IDENT in/of expression) statement
         for (var/let/const varDeclarationList; expressionOpt; expressionOpt)
         */
        if (isLetDeclaration || isConstDeclaration) {
            ScopeRef newScope = pushScope();
            newScope->setIsLexicalScope();
            newScope->preventVarDeclarations();
            lexicalScope.setIsValid(newScope, this);
        }

        TreeDestructuringPattern forInTarget = 0;
        TreeExpression forInInitializer = 0;
        m_allowsIn = false;
        JSTextPosition initStart;
        JSTextPosition initEnd;
        DeclarationType declarationType;
        if (isVarDeclaraton)
            declarationType = DeclarationType::VarDeclaration;
        else if (isLetDeclaration)
            declarationType = DeclarationType::LetDeclaration;
        else if (isConstDeclaration)
            declarationType = DeclarationType::ConstDeclaration;
        else
            RELEASE_ASSERT_NOT_REACHED();
        decls = parseVariableDeclarationList(context, declarations, forInTarget, forInInitializer, declsStart, initStart, initEnd, ForLoopContext, declarationType, ExportType::NotExported, forLoopConstDoesNotHaveInitializer);
        m_allowsIn = true;
        propagateError();

        // Remainder of a standard for loop is handled identically
        if (match(SEMICOLON))
            goto standardForLoop;
        
        failIfFalse(declarations == 1, "can only declare a single variable in an enumeration");
        failIfTrueIfStrict(forInInitializer, "Cannot use initialiser syntax in a strict mode enumeration");

        if (forInInitializer)
            failIfFalse(context.isBindingNode(forInTarget), "Cannot use initialiser syntax when binding to a pattern during enumeration");

        // Handle for-in with var declaration
        JSTextPosition inLocation = tokenStartPosition();
        bool isOfEnumeration = false;
        if (!consume(INTOKEN)) {
            failIfFalse(match(IDENT) && *m_token.m_data.ident == m_vm->propertyNames->of, "Expected either 'in' or 'of' in enumeration syntax");
            isOfEnumeration = true;
            failIfTrue(forInInitializer, "Cannot use initialiser syntax in a for-of enumeration");
            next();
        }
        TreeExpression expr = parseExpression(context);
        failIfFalse(expr, "Expected expression to enumerate");
        JSTextPosition exprEnd = lastTokenEndPosition();
        
        int endLine = tokenLine();
        
        handleProductionOrFail(CLOSEPAREN, ")", "end", (isOfEnumeration ? "for-of header" : "for-in header"));
        
        const Identifier* unused = 0;
        startLoop();
        TreeStatement statement = parseStatement(context, unused);
        endLoop();
        failIfFalse(statement, "Expected statement as body of for-", isOfEnumeration ? "of" : "in", " statement");
        gatherLexicalVariablesIfNecessary();
        TreeStatement result;
        if (isOfEnumeration)
            result = context.createForOfLoop(location, forInTarget, expr, statement, declsStart, inLocation, exprEnd, startLine, endLine, *lexicalVariables);
        else
            result = context.createForInLoop(location, forInTarget, expr, statement, declsStart, inLocation, exprEnd, startLine, endLine, *lexicalVariables);
        popLexicalScopeIfNecessary();
        return result;
    }
    
    if (!match(SEMICOLON)) {
        if (match(OPENBRACE) || match(OPENBRACKET)) {
            SavePoint savePoint = createSavePoint();
            declsStart = tokenStartPosition();
            pattern = tryParseDestructuringPatternExpression(context, AssignmentContext::DeclarationStatement);
            declsEnd = lastTokenEndPosition();
            if (pattern && (match(INTOKEN) || (match(IDENT) && *m_token.m_data.ident == m_vm->propertyNames->of)))
                goto enumerationLoop;
            pattern = TreeDestructuringPattern(0);
            restoreSavePoint(savePoint);
        }
        m_allowsIn = false;
        declsStart = tokenStartPosition();
        decls = parseExpression(context);
        declsEnd = lastTokenEndPosition();
        m_allowsIn = true;
        failIfFalse(decls, "Cannot parse for loop declarations");
    }
    
    if (match(SEMICOLON)) {
    standardForLoop:
        // Standard for loop
        next();
        TreeExpression condition = 0;
        failIfTrue(forLoopConstDoesNotHaveInitializer && isConstDeclaration, "const variables in for loops must have initializers");
        
        if (!match(SEMICOLON)) {
            condition = parseExpression(context);
            failIfFalse(condition, "Cannot parse for loop condition expression");
        }
        consumeOrFail(SEMICOLON, "Expected a ';' after the for loop condition expression");
        
        TreeExpression increment = 0;
        if (!match(CLOSEPAREN)) {
            increment = parseExpression(context);
            failIfFalse(increment, "Cannot parse for loop iteration expression");
        }
        int endLine = tokenLine();
        handleProductionOrFail(CLOSEPAREN, ")", "end", "for-loop header");
        const Identifier* unused = 0;
        startLoop();
        TreeStatement statement = parseStatement(context, unused);
        endLoop();
        failIfFalse(statement, "Expected a statement as the body of a for loop");
        gatherLexicalVariablesIfNecessary();
        TreeStatement result = context.createForLoop(location, decls, condition, increment, statement, startLine, endLine, *lexicalVariables);
        popLexicalScopeIfNecessary();
        return result;
    }
    
    // For-in and For-of loop
enumerationLoop:
    failIfFalse(nonLHSCount == m_parserState.nonLHSCount, "Expected a reference on the left hand side of an enumeration statement");
    bool isOfEnumeration = false;
    if (!consume(INTOKEN)) {
        failIfFalse(match(IDENT) && *m_token.m_data.ident == m_vm->propertyNames->of, "Expected either 'in' or 'of' in enumeration syntax");
        isOfEnumeration = true;
        next();
    }
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse subject for-", isOfEnumeration ? "of" : "in", " statement");
    JSTextPosition exprEnd = lastTokenEndPosition();
    int endLine = tokenLine();
    
    handleProductionOrFail(CLOSEPAREN, ")", "end", (isOfEnumeration ? "for-of header" : "for-in header"));
    const Identifier* unused = 0;
    startLoop();
    TreeStatement statement = parseStatement(context, unused);
    endLoop();
    failIfFalse(statement, "Expected a statement as the body of a for-", isOfEnumeration ? "of" : "in", "loop");
    gatherLexicalVariablesIfNecessary();
    TreeStatement result;
    if (pattern) {
        ASSERT(!decls);
        if (isOfEnumeration)
            result = context.createForOfLoop(location, pattern, expr, statement, declsStart, declsEnd, exprEnd, startLine, endLine, *lexicalVariables);
        else 
            result = context.createForInLoop(location, pattern, expr, statement, declsStart, declsEnd, exprEnd, startLine, endLine, *lexicalVariables);

        popLexicalScopeIfNecessary();
        return result;
    }
    if (isOfEnumeration)
        result = context.createForOfLoop(location, decls, expr, statement, declsStart, declsEnd, exprEnd, startLine, endLine, *lexicalVariables);
    else
        result = context.createForInLoop(location, decls, expr, statement, declsStart, declsEnd, exprEnd, startLine, endLine, *lexicalVariables);
    popLexicalScopeIfNecessary();
    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseBreakStatement(TreeBuilder& context)
{
    ASSERT(match(BREAK));
    JSTokenLocation location(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    JSTextPosition end = tokenEndPosition();
    next();
    
    if (autoSemiColon()) {
        semanticFailIfFalse(breakIsValid(), "'break' is only valid inside a switch or loop statement");
        return context.createBreakStatement(location, &m_vm->propertyNames->nullIdentifier, start, end);
    }
    failIfFalse(matchSpecIdentifier(), "Expected an identifier as the target for a break statement");
    const Identifier* ident = m_token.m_data.ident;
    semanticFailIfFalse(getLabel(ident), "Cannot use the undeclared label '", ident->impl(), "'");
    end = tokenEndPosition();
    next();
    failIfFalse(autoSemiColon(), "Expected a ';' following a targeted break statement");
    return context.createBreakStatement(location, ident, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseContinueStatement(TreeBuilder& context)
{
    ASSERT(match(CONTINUE));
    JSTokenLocation location(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    JSTextPosition end = tokenEndPosition();
    next();
    
    if (autoSemiColon()) {
        semanticFailIfFalse(continueIsValid(), "'continue' is only valid inside a loop statement");
        return context.createContinueStatement(location, &m_vm->propertyNames->nullIdentifier, start, end);
    }
    failIfFalse(matchSpecIdentifier(), "Expected an identifier as the target for a continue statement");
    const Identifier* ident = m_token.m_data.ident;
    ScopeLabelInfo* label = getLabel(ident);
    semanticFailIfFalse(label, "Cannot use the undeclared label '", ident->impl(), "'");
    semanticFailIfFalse(label->isLoop, "Cannot continue to the label '", ident->impl(), "' as it is not targeting a loop");
    end = tokenEndPosition();
    next();
    failIfFalse(autoSemiColon(), "Expected a ';' following a targeted continue statement");
    return context.createContinueStatement(location, ident, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseReturnStatement(TreeBuilder& context)
{
    ASSERT(match(RETURN));
    JSTokenLocation location(tokenLocation());
    semanticFailIfFalse(currentScope()->isFunction(), "Return statements are only valid inside functions");
    JSTextPosition start = tokenStartPosition();
    JSTextPosition end = tokenEndPosition();
    next();
    // We do the auto semicolon check before attempting to parse expression
    // as we need to ensure the a line break after the return correctly terminates
    // the statement
    if (match(SEMICOLON))
        end = tokenEndPosition();

    if (autoSemiColon())
        return context.createReturnStatement(location, 0, start, end);
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse the return expression");
    end = lastTokenEndPosition();
    if (match(SEMICOLON))
        end  = tokenEndPosition();
    if (!autoSemiColon())
        failWithMessage("Expected a ';' following a return statement");
    return context.createReturnStatement(location, expr, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseThrowStatement(TreeBuilder& context)
{
    ASSERT(match(THROW));
    JSTokenLocation location(tokenLocation());
    JSTextPosition start = tokenStartPosition();
    next();
    failIfTrue(match(SEMICOLON), "Expected expression after 'throw'");
    semanticFailIfTrue(autoSemiColon(), "Cannot have a newline after 'throw'");
    
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse expression for throw statement");
    JSTextPosition end = lastTokenEndPosition();
    failIfFalse(autoSemiColon(), "Expected a ';' after a throw statement");
    
    return context.createThrowStatement(location, expr, start, end);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseWithStatement(TreeBuilder& context)
{
    ASSERT(match(WITH));
    JSTokenLocation location(tokenLocation());
    semanticFailIfTrue(strictMode(), "'with' statements are not valid in strict mode");
    currentScope()->setNeedsFullActivation();
    int startLine = tokenLine();
    next();

    handleProductionOrFail(OPENPAREN, "(", "start", "subject of a 'with' statement");
    int start = tokenStart();
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse 'with' subject expression");
    JSTextPosition end = lastTokenEndPosition();
    int endLine = tokenLine();
    handleProductionOrFail(CLOSEPAREN, ")", "start", "subject of a 'with' statement");
    const Identifier* unused = 0;
    TreeStatement statement = parseStatement(context, unused);
    failIfFalse(statement, "A 'with' statement must have a body");
    
    return context.createWithStatement(location, expr, statement, start, end, startLine, endLine);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseSwitchStatement(TreeBuilder& context)
{
    ASSERT(match(SWITCH));
    JSTokenLocation location(tokenLocation());
    int startLine = tokenLine();
    next();
    handleProductionOrFail(OPENPAREN, "(", "start", "subject of a 'switch'");
    TreeExpression expr = parseExpression(context);
    failIfFalse(expr, "Cannot parse switch subject expression");
    int endLine = tokenLine();
    
    handleProductionOrFail(CLOSEPAREN, ")", "end", "subject of a 'switch'");
    handleProductionOrFail(OPENBRACE, "{", "start", "body of a 'switch'");
    AutoPopScopeRef lexicalScope(this, pushScope());
    lexicalScope->setIsLexicalScope();
    lexicalScope->preventVarDeclarations();
    startSwitch();
    TreeClauseList firstClauses = parseSwitchClauses(context);
    propagateError();
    
    TreeClause defaultClause = parseSwitchDefaultClause(context);
    propagateError();
    
    TreeClauseList secondClauses = parseSwitchClauses(context);
    propagateError();
    endSwitch();
    handleProductionOrFail(CLOSEBRACE, "}", "end", "body of a 'switch'");
    
    TreeStatement result = context.createSwitchStatement(location, expr, firstClauses, defaultClause, secondClauses, startLine, endLine, lexicalScope->finalizeLexicalEnvironment());
    popScope(lexicalScope, TreeBuilder::NeedsFreeVariableInfo);
    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeClauseList Parser<LexerType>::parseSwitchClauses(TreeBuilder& context)
{
    if (!match(CASE))
        return 0;
    unsigned startOffset = tokenStart();
    next();
    TreeExpression condition = parseExpression(context);
    failIfFalse(condition, "Cannot parse switch clause");
    consumeOrFail(COLON, "Expected a ':' after switch clause expression");
    TreeSourceElements statements = parseSourceElements(context, DontCheckForStrictMode);
    failIfFalse(statements, "Cannot parse the body of a switch clause");
    TreeClause clause = context.createClause(condition, statements);
    context.setStartOffset(clause, startOffset);
    TreeClauseList clauseList = context.createClauseList(clause);
    TreeClauseList tail = clauseList;
    
    while (match(CASE)) {
        startOffset = tokenStart();
        next();
        TreeExpression condition = parseExpression(context);
        failIfFalse(condition, "Cannot parse switch case expression");
        consumeOrFail(COLON, "Expected a ':' after switch clause expression");
        TreeSourceElements statements = parseSourceElements(context, DontCheckForStrictMode);
        failIfFalse(statements, "Cannot parse the body of a switch clause");
        clause = context.createClause(condition, statements);
        context.setStartOffset(clause, startOffset);
        tail = context.createClauseList(tail, clause);
    }
    return clauseList;
}

template <typename LexerType>
template <class TreeBuilder> TreeClause Parser<LexerType>::parseSwitchDefaultClause(TreeBuilder& context)
{
    if (!match(DEFAULT))
        return 0;
    unsigned startOffset = tokenStart();
    next();
    consumeOrFail(COLON, "Expected a ':' after switch default clause");
    TreeSourceElements statements = parseSourceElements(context, DontCheckForStrictMode);
    failIfFalse(statements, "Cannot parse the body of a switch default clause");
    TreeClause result = context.createClause(0, statements);
    context.setStartOffset(result, startOffset);
    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseTryStatement(TreeBuilder& context)
{
    ASSERT(match(TRY));
    JSTokenLocation location(tokenLocation());
    TreeStatement tryBlock = 0;
    TreeDestructuringPattern catchPattern = 0;
    TreeStatement catchBlock = 0;
    TreeStatement finallyBlock = 0;
    int firstLine = tokenLine();
    next();
    matchOrFail(OPENBRACE, "Expected a block statement as body of a try statement");
    
    tryBlock = parseBlockStatement(context);
    failIfFalse(tryBlock, "Cannot parse the body of try block");
    int lastLine = m_lastTokenEndPosition.line;
    VariableEnvironment catchEnvironment; 
    if (match(CATCH)) {
        next();
        
        handleProductionOrFail(OPENPAREN, "(", "start", "'catch' target");
        AutoPopScopeRef catchScope(this, pushScope());
        catchScope->setIsLexicalScope();
        catchScope->preventVarDeclarations();
        const Identifier* ident = nullptr;
        if (matchSpecIdentifier()) {
            ident = m_token.m_data.ident;
            catchPattern = context.createBindingLocation(m_token.m_location, *ident, m_token.m_startPosition, m_token.m_endPosition, AssignmentContext::DeclarationStatement);
            next();
            failIfTrueIfStrict(catchScope->declareLexicalVariable(ident, false) & DeclarationResult::InvalidStrictMode, "Cannot declare a catch variable named '", ident->impl(), "' in strict mode");
        } else {
            catchPattern = parseDestructuringPattern(context, DestructuringKind::DestructureToCatchParameters, ExportType::NotExported);
            failIfFalse(catchPattern, "Cannot parse this destructuring pattern");
        }
        handleProductionOrFail(CLOSEPAREN, ")", "end", "'catch' target");
        matchOrFail(OPENBRACE, "Expected exception handler to be a block statement");
        catchBlock = parseBlockStatement(context);
        failIfFalse(catchBlock, "Unable to parse 'catch' block");
        catchEnvironment = catchScope->finalizeLexicalEnvironment();
        RELEASE_ASSERT(!ident || (catchEnvironment.size() == 1 && catchEnvironment.contains(ident->impl())));
        popScope(catchScope, TreeBuilder::NeedsFreeVariableInfo);
    }
    
    if (match(FINALLY)) {
        next();
        matchOrFail(OPENBRACE, "Expected block statement for finally body");
        finallyBlock = parseBlockStatement(context);
        failIfFalse(finallyBlock, "Cannot parse finally body");
    }
    failIfFalse(catchBlock || finallyBlock, "Try statements must have at least a catch or finally block");
    return context.createTryStatement(location, tryBlock, catchPattern, catchBlock, finallyBlock, firstLine, lastLine, catchEnvironment);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseDebuggerStatement(TreeBuilder& context)
{
    ASSERT(match(DEBUGGER));
    JSTokenLocation location(tokenLocation());
    int startLine = tokenLine();
    int endLine = startLine;
    next();
    if (match(SEMICOLON))
        startLine = tokenLine();
    failIfFalse(autoSemiColon(), "Debugger keyword must be followed by a ';'");
    return context.createDebugger(location, startLine, endLine);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseBlockStatement(TreeBuilder& context)
{
    ASSERT(match(OPENBRACE));

    // We should treat the first block statement of the function (the body of the function) as the lexical 
    // scope of the function itself, and not the lexical scope of a 'block' statement within the function.
    AutoCleanupLexicalScope lexicalScope;
    bool shouldPushLexicalScope = m_statementDepth > 0;
    if (shouldPushLexicalScope) {
        ScopeRef newScope = pushScope();
        newScope->setIsLexicalScope();
        newScope->preventVarDeclarations();
        lexicalScope.setIsValid(newScope, this);
    }
    JSTokenLocation location(tokenLocation());
    int startOffset = m_token.m_data.offset;
    int start = tokenLine();
    VariableEnvironment emptyEnvironment;
    next();
    if (match(CLOSEBRACE)) {
        int endOffset = m_token.m_data.offset;
        next();
        TreeStatement result = context.createBlockStatement(location, 0, start, m_lastTokenEndPosition.line, shouldPushLexicalScope ? currentScope()->finalizeLexicalEnvironment() : emptyEnvironment);
        context.setStartOffset(result, startOffset);
        context.setEndOffset(result, endOffset);
        if (shouldPushLexicalScope)
            popScope(lexicalScope, TreeBuilder::NeedsFreeVariableInfo);
        return result;
    }
    TreeSourceElements subtree = parseSourceElements(context, DontCheckForStrictMode);
    failIfFalse(subtree, "Cannot parse the body of the block statement");
    matchOrFail(CLOSEBRACE, "Expected a closing '}' at the end of a block statement");
    int endOffset = m_token.m_data.offset;
    next();
    TreeStatement result = context.createBlockStatement(location, subtree, start, m_lastTokenEndPosition.line, shouldPushLexicalScope ? currentScope()->finalizeLexicalEnvironment() : emptyEnvironment);
    context.setStartOffset(result, startOffset);
    context.setEndOffset(result, endOffset);
    if (shouldPushLexicalScope)
        popScope(lexicalScope, TreeBuilder::NeedsFreeVariableInfo);

    return result;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseStatement(TreeBuilder& context, const Identifier*& directive, unsigned* directiveLiteralLength)
{
    DepthManager statementDepth(&m_statementDepth);
    m_statementDepth++;
    directive = 0;
    int nonTrivialExpressionCount = 0;
    failIfStackOverflow();
    TreeStatement result = 0;
    bool shouldSetEndOffset = true;

    switch (m_token.m_type) {
    case OPENBRACE:
        result = parseBlockStatement(context);
        shouldSetEndOffset = false;
        break;
    case VAR:
        result = parseVariableDeclaration(context, DeclarationType::VarDeclaration);
        break;
    case FUNCTION:
        failIfFalseIfStrict(m_statementDepth == 1, "Strict mode does not allow function declarations in a lexically nested statement");
        result = parseFunctionDeclaration(context);
        break;
    case SEMICOLON: {
        JSTokenLocation location(tokenLocation());
        next();
        result = context.createEmptyStatement(location);
        break;
    }
    case IF:
        result = parseIfStatement(context);
        break;
    case DO:
        result = parseDoWhileStatement(context);
        break;
    case WHILE:
        result = parseWhileStatement(context);
        break;
    case FOR:
        result = parseForStatement(context);
        break;
    case CONTINUE:
        result = parseContinueStatement(context);
        break;
    case BREAK:
        result = parseBreakStatement(context);
        break;
    case RETURN:
        result = parseReturnStatement(context);
        break;
    case WITH:
        result = parseWithStatement(context);
        break;
    case SWITCH:
        result = parseSwitchStatement(context);
        break;
    case THROW:
        result = parseThrowStatement(context);
        break;
    case TRY:
        result = parseTryStatement(context);
        break;
    case DEBUGGER:
        result = parseDebuggerStatement(context);
        break;
    case EOFTOK:
    case CASE:
    case CLOSEBRACE:
    case DEFAULT:
        // These tokens imply the end of a set of source elements
        return 0;
    case IDENT:
    case YIELD:
        result = parseExpressionOrLabelStatement(context);
        break;
    case STRING:
        directive = m_token.m_data.ident;
        if (directiveLiteralLength)
            *directiveLiteralLength = m_token.m_location.endOffset - m_token.m_location.startOffset;
        nonTrivialExpressionCount = m_parserState.nonTrivialExpressionCount;
        FALLTHROUGH;
    default:
        TreeStatement exprStatement = parseExpressionStatement(context);
        if (directive && nonTrivialExpressionCount != m_parserState.nonTrivialExpressionCount)
            directive = 0;
        result = exprStatement;
        break;
    }

    if (result && shouldSetEndOffset)
        context.setEndOffset(result, m_lastTokenEndPosition.offset);
    return result;
}

template <typename LexerType>
template <class TreeBuilder> bool Parser<LexerType>::parseFormalParameters(TreeBuilder& context, TreeFormalParameterList list, unsigned& parameterCount)
{
#define failIfDuplicateIfViolation() \
    if (duplicateParameter) {\
        semanticFailIfTrue(defaultValue, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in function with default parameter values");\
        semanticFailIfTrue(hasDestructuringPattern, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in function with destructuring parameters");\
        semanticFailIfTrue(isRestParameter, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in function with a rest parameter");\
    }

    bool hasDestructuringPattern = false;
    bool isRestParameter = false;
    const Identifier* duplicateParameter = nullptr;
    do {
        TreeDestructuringPattern parameter = 0;
        TreeExpression defaultValue = 0;

        if (match(DOTDOTDOT)) {
            next();
            failIfFalse(matchSpecIdentifier(), "Rest parameter '...' should be followed by a variable identifier");
            declareRestOrNormalParameter(*m_token.m_data.ident, &duplicateParameter);
            propagateError();
            JSTextPosition identifierStart = tokenStartPosition();
            JSTextPosition identifierEnd = tokenEndPosition();
            parameter = context.createRestParameter(*m_token.m_data.ident, parameterCount, identifierStart, identifierEnd);
            next();
            failIfTrue(match(COMMA), "Rest parameter should be the last parameter in a function declaration"); // Let's have a good error message for this common case.
            isRestParameter = true;
        } else
            parameter = parseDestructuringPattern(context, DestructuringKind::DestructureToParameters, ExportType::NotExported, &duplicateParameter, &hasDestructuringPattern);
        failIfFalse(parameter, "Cannot parse parameter pattern");
        if (!isRestParameter)
            defaultValue = parseDefaultValueForDestructuringPattern(context);
        propagateError();
        failIfDuplicateIfViolation();
        context.appendParameter(list, parameter, defaultValue);
        if (!isRestParameter)
            parameterCount++;
    } while (!isRestParameter && consume(COMMA));

    return true;
#undef failIfDuplicateIfViolation
}

template <typename LexerType>
template <class TreeBuilder> TreeFunctionBody Parser<LexerType>::parseFunctionBody(
    TreeBuilder& context, const JSTokenLocation& startLocation, int startColumn, int functionKeywordStart, int functionNameStart, int parametersStart, 
    ConstructorKind constructorKind, SuperBinding superBinding, FunctionBodyType bodyType, unsigned parameterCount, SourceParseMode parseMode)
{
    bool isArrowFunctionBodyExpression = bodyType == ArrowFunctionBodyExpression;
    if (!isArrowFunctionBodyExpression) {
        next();
        if (match(CLOSEBRACE)) {
            unsigned endColumn = tokenColumn();
            return context.createFunctionMetadata(startLocation, tokenLocation(), startColumn, endColumn, functionKeywordStart, functionNameStart, parametersStart, strictMode(), constructorKind, superBinding, parameterCount, parseMode, isArrowFunctionBodyExpression);
        }
    }

    DepthManager statementDepth(&m_statementDepth);
    m_statementDepth = 0;
    SyntaxChecker syntaxChecker(const_cast<VM*>(m_vm), m_lexer.get());
    if (bodyType == ArrowFunctionBodyExpression)
        failIfFalse(parseArrowFunctionSingleExpressionBodySourceElements(syntaxChecker), "Cannot parse body of this arrow function");
    else
        failIfFalse(parseSourceElements(syntaxChecker, CheckForStrictMode), bodyType == StandardFunctionBodyBlock ? "Cannot parse body of this function" : "Cannot parse body of this arrow function");
    unsigned endColumn = tokenColumn();
    return context.createFunctionMetadata(startLocation, tokenLocation(), startColumn, endColumn, functionKeywordStart, functionNameStart, parametersStart, strictMode(), constructorKind, superBinding, parameterCount, parseMode, isArrowFunctionBodyExpression);
}

static const char* stringForFunctionMode(SourceParseMode mode)
{
    switch (mode) {
    case SourceParseMode::GetterMode:
        return "getter";
    case SourceParseMode::SetterMode:
        return "setter";
    case SourceParseMode::NormalFunctionMode:
        return "function";
    case SourceParseMode::MethodMode:
        return "method";
    case SourceParseMode::GeneratorBodyMode:
        return "generator";
    case SourceParseMode::GeneratorWrapperFunctionMode:
        return "generator function";
    case SourceParseMode::ArrowFunctionMode:
        return "arrow function";
    case SourceParseMode::ProgramMode:
    case SourceParseMode::ModuleAnalyzeMode:
    case SourceParseMode::ModuleEvaluateMode:
        RELEASE_ASSERT_NOT_REACHED();
        return "";
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

template <typename LexerType> template <class TreeBuilder> int Parser<LexerType>::parseFunctionParameters(TreeBuilder& context, SourceParseMode mode, ParserFunctionInfo<TreeBuilder>& functionInfo)
{
    RELEASE_ASSERT(mode != SourceParseMode::ProgramMode && mode != SourceParseMode::ModuleAnalyzeMode && mode != SourceParseMode::ModuleEvaluateMode);
    int parametersStart = m_token.m_location.startOffset;
    TreeFormalParameterList parameterList = context.createFormalParameterList();
    functionInfo.parameters = parameterList;
    functionInfo.startOffset = parametersStart;
    SetForScope<FunctionParsePhase> functionParsePhasePoisoner(m_parserState.functionParsePhase, FunctionParsePhase::Parameters);
    
    if (mode == SourceParseMode::ArrowFunctionMode) {
        if (!match(IDENT) && !match(OPENPAREN)) {
            semanticFailureDueToKeyword(stringForFunctionMode(mode), " name");
            failWithMessage("Expected an arrow function input parameter");
        } else {
            if (match(OPENPAREN)) {
                next();
                
                if (match(CLOSEPAREN))
                    functionInfo.parameterCount = 0;
                else
                    failIfFalse(parseFormalParameters(context, parameterList, functionInfo.parameterCount), "Cannot parse parameters for this ", stringForFunctionMode(mode));
                
                consumeOrFail(CLOSEPAREN, "Expected a ')' or a ',' after a parameter declaration");
            } else {
                functionInfo.parameterCount = 1;
                auto parameter = parseDestructuringPattern(context, DestructuringKind::DestructureToParameters, ExportType::NotExported);
                failIfFalse(parameter, "Cannot parse parameter pattern");
                context.appendParameter(parameterList, parameter, 0);
            }
        }

        return parametersStart;
    }

    if (!consume(OPENPAREN)) {
        semanticFailureDueToKeyword(stringForFunctionMode(mode), " name");
        failWithMessage("Expected an opening '(' before a ", stringForFunctionMode(mode), "'s parameter list");
    }

    if (mode == SourceParseMode::GetterMode) {
        consumeOrFail(CLOSEPAREN, "getter functions must have no parameters");
        functionInfo.parameterCount = 0;
    } else if (mode == SourceParseMode::SetterMode) {
        failIfTrue(match(CLOSEPAREN), "setter functions must have one parameter");
        const Identifier* duplicateParameter = nullptr;
        auto parameter = parseDestructuringPattern(context, DestructuringKind::DestructureToParameters, ExportType::NotExported, &duplicateParameter);
        failIfFalse(parameter, "setter functions must have one parameter");
        auto defaultValue = parseDefaultValueForDestructuringPattern(context);
        propagateError();
        semanticFailIfTrue(duplicateParameter && defaultValue, "Duplicate parameter '", duplicateParameter->impl(), "' not allowed in function with default parameter values");
        context.appendParameter(parameterList, parameter, defaultValue);
        functionInfo.parameterCount = 1;
        failIfTrue(match(COMMA), "setter functions must have one parameter");
        consumeOrFail(CLOSEPAREN, "Expected a ')' after a parameter declaration");
    } else {
        if (match(CLOSEPAREN))
            functionInfo.parameterCount = 0;
        else
            failIfFalse(parseFormalParameters(context, parameterList, functionInfo.parameterCount), "Cannot parse parameters for this ", stringForFunctionMode(mode));
        consumeOrFail(CLOSEPAREN, "Expected a ')' or a ',' after a parameter declaration");
    }

    return parametersStart;
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::FormalParameterList Parser<LexerType>::createGeneratorParameters(TreeBuilder& context)
{
    auto parameters = context.createFormalParameterList();

    JSTokenLocation location(tokenLocation());
    JSTextPosition position = tokenStartPosition();

    // @generator
    declareParameter(&m_vm->propertyNames->generatorPrivateName);
    auto generator = context.createBindingLocation(location, m_vm->propertyNames->generatorPrivateName, position, position, AssignmentContext::DeclarationStatement);
    context.appendParameter(parameters, generator, 0);

    // @generatorState
    declareParameter(&m_vm->propertyNames->generatorStatePrivateName);
    auto generatorState = context.createBindingLocation(location, m_vm->propertyNames->generatorStatePrivateName, position, position, AssignmentContext::DeclarationStatement);
    context.appendParameter(parameters, generatorState, 0);

    // @generatorValue
    declareParameter(&m_vm->propertyNames->generatorValuePrivateName);
    auto generatorValue = context.createBindingLocation(location, m_vm->propertyNames->generatorValuePrivateName, position, position, AssignmentContext::DeclarationStatement);
    context.appendParameter(parameters, generatorValue, 0);

    // @generatorResumeMode
    declareParameter(&m_vm->propertyNames->generatorResumeModePrivateName);
    auto generatorResumeMode = context.createBindingLocation(location, m_vm->propertyNames->generatorResumeModePrivateName, position, position, AssignmentContext::DeclarationStatement);
    context.appendParameter(parameters, generatorResumeMode, 0);

    return parameters;
}

template <typename LexerType>
template <class TreeBuilder> bool Parser<LexerType>::parseFunctionInfo(TreeBuilder& context, FunctionRequirements requirements, SourceParseMode mode, bool nameIsInContainingScope, ConstructorKind constructorKind, SuperBinding expectedSuperBinding, int functionKeywordStart, ParserFunctionInfo<TreeBuilder>& functionInfo, FunctionDefinitionType functionDefinitionType)
{
    RELEASE_ASSERT(isFunctionParseMode(mode));

    bool upperScopeIsGenerator = currentScope()->isGenerator();
    AutoPopScopeRef functionScope(this, pushScope());
    functionScope->setSourceParseMode(mode);
    SetForScope<FunctionParsePhase> functionParsePhasePoisoner(m_parserState.functionParsePhase, FunctionParsePhase::Body);
    int functionNameStart = m_token.m_location.startOffset;
    const Identifier* lastFunctionName = m_parserState.lastFunctionName;
    m_parserState.lastFunctionName = nullptr;
    int parametersStart;
    JSTokenLocation startLocation;
    int startColumn;
    FunctionBodyType functionBodyType;

    if (mode == SourceParseMode::ArrowFunctionMode) {
        startLocation = tokenLocation();
        functionInfo.startLine = tokenLine();
        startColumn = tokenColumn();

        parametersStart = parseFunctionParameters(context, mode, functionInfo);
        propagateError();

        matchOrFail(ARROWFUNCTION, "Expected a '=>' after arrow function parameter declaration");

        if (m_lexer->prevTerminator())
            failDueToUnexpectedToken();

        ASSERT(constructorKind == ConstructorKind::None);

        // Check if arrow body start with {. If it true it mean that arrow function is Fat arrow function
        // and we need use common approach to parse function body
        next();
        functionBodyType = match(OPENBRACE) ? ArrowFunctionBodyBlock : ArrowFunctionBodyExpression;
    } else {
        // http://ecma-international.org/ecma-262/6.0/#sec-function-definitions
        // FunctionExpression :
        //     function BindingIdentifieropt ( FormalParameters ) { FunctionBody }
        //
        // FunctionDeclaration[Yield, Default] :
        //     function BindingIdentifier[?Yield] ( FormalParameters ) { FunctionBody }
        //     [+Default] function ( FormalParameters ) { FunctionBody }
        //
        // GeneratorDeclaration[Yield, Default] :
        //     function * BindingIdentifier[?Yield] ( FormalParameters[Yield] ) { GeneratorBody }
        //     [+Default] function * ( FormalParameters[Yield] ) { GeneratorBody }
        //
        // GeneratorExpression :
        //     function * BindingIdentifier[Yield]opt ( FormalParameters[Yield] ) { GeneratorBody }
        //
        // The name of FunctionExpression can accept "yield" even in the context of generator.
        if (functionDefinitionType == FunctionDefinitionType::Expression && mode == SourceParseMode::NormalFunctionMode)
            upperScopeIsGenerator = false;

        if (matchSpecIdentifier(upperScopeIsGenerator)) {
            functionInfo.name = m_token.m_data.ident;
            m_parserState.lastFunctionName = functionInfo.name;
            next();
            if (!nameIsInContainingScope)
                failIfTrueIfStrict(functionScope->declareCallee(functionInfo.name) & DeclarationResult::InvalidStrictMode, "'", functionInfo.name->impl(), "' is not a valid ", stringForFunctionMode(mode), " name in strict mode");
        } else if (requirements == FunctionNeedsName) {
            if (match(OPENPAREN) && mode == SourceParseMode::NormalFunctionMode)
                semanticFail("Function statements must have a name");
            semanticFailureDueToKeyword(stringForFunctionMode(mode), " name");
            failDueToUnexpectedToken();
            return false;
        }

        startLocation = tokenLocation();
        functionInfo.startLine = tokenLine();
        startColumn = tokenColumn();

        parametersStart = parseFunctionParameters(context, mode, functionInfo);
        propagateError();
        
        matchOrFail(OPENBRACE, "Expected an opening '{' at the start of a ", stringForFunctionMode(mode), " body");
        
        // BytecodeGenerator emits code to throw TypeError when a class constructor is "call"ed.
        // Set ConstructorKind to None for non-constructor methods of classes.
    
        if (m_defaultConstructorKind != ConstructorKind::None) {
            constructorKind = m_defaultConstructorKind;
            expectedSuperBinding = m_defaultConstructorKind == ConstructorKind::Derived ? SuperBinding::Needed : SuperBinding::NotNeeded;
        }

        functionBodyType = StandardFunctionBodyBlock;
    }

    functionScope->setConstructorKind(constructorKind);
    functionScope->setExpectedSuperBinding(expectedSuperBinding);

    functionInfo.bodyStartColumn = startColumn;
    
    // If we know about this function already, we can use the cached info and skip the parser to the end of the function.
    if (const SourceProviderCacheItem* cachedInfo = TreeBuilder::CanUseFunctionCache ? findCachedFunctionInfo(functionInfo.startOffset) : 0) {
        // If we're in a strict context, the cached function info must say it was strict too.
        ASSERT(!strictMode() || cachedInfo->strictMode);
        JSTokenLocation endLocation;

        endLocation.line = cachedInfo->lastTockenLine;
        endLocation.startOffset = cachedInfo->lastTockenStartOffset;
        endLocation.lineStartOffset = cachedInfo->lastTockenLineStartOffset;

        bool endColumnIsOnStartLine = (endLocation.line == functionInfo.startLine);
        ASSERT(endLocation.startOffset >= endLocation.lineStartOffset);
        unsigned bodyEndColumn = endColumnIsOnStartLine ?
            endLocation.startOffset - m_token.m_data.lineStartOffset :
            endLocation.startOffset - endLocation.lineStartOffset;
        unsigned currentLineStartOffset = m_token.m_location.lineStartOffset;
        
        functionInfo.body = context.createFunctionMetadata(
            startLocation, endLocation, functionInfo.bodyStartColumn, bodyEndColumn, 
            functionKeywordStart, functionNameStart, parametersStart, 
            cachedInfo->strictMode, constructorKind, expectedSuperBinding, cachedInfo->parameterCount, mode, functionBodyType == ArrowFunctionBodyExpression);
        
        functionScope->restoreFromSourceProviderCache(cachedInfo);
        popScope(functionScope, TreeBuilder::NeedsFreeVariableInfo);
        
        m_token = cachedInfo->endFunctionToken();
        
        if (endColumnIsOnStartLine)
            m_token.m_location.lineStartOffset = currentLineStartOffset;

        m_lexer->setOffset(m_token.m_location.endOffset, m_token.m_location.lineStartOffset);
        m_lexer->setLineNumber(m_token.m_location.line);
        functionInfo.endOffset = cachedInfo->endFunctionOffset;

        if (mode == SourceParseMode::ArrowFunctionMode)
            functionBodyType = cachedInfo->isBodyArrowExpression ?  ArrowFunctionBodyExpression : ArrowFunctionBodyBlock;
        else
            functionBodyType = StandardFunctionBodyBlock;
        
        switch (functionBodyType) {
        case ArrowFunctionBodyExpression:
            next();
            context.setEndOffset(functionInfo.body, m_lexer->currentOffset());
            break;
        case ArrowFunctionBodyBlock:
        case StandardFunctionBodyBlock:
            context.setEndOffset(functionInfo.body, m_lexer->currentOffset());
            next();
            break;
        }
        functionInfo.endLine = m_lastTokenEndPosition.line;
        return true;
    }
    
    m_parserState.lastFunctionName = lastFunctionName;
    ParserState oldState = internalSaveParserState();

    auto performParsingFunctionBody = [&] {
        return parseFunctionBody(context, startLocation, startColumn, functionKeywordStart, functionNameStart, parametersStart, constructorKind, expectedSuperBinding, functionBodyType, functionInfo.parameterCount, mode);
    };

    if (mode == SourceParseMode::GeneratorWrapperFunctionMode) {
        AutoPopScopeRef generatorBodyScope(this, pushScope());
        generatorBodyScope->setSourceParseMode(SourceParseMode::GeneratorBodyMode);
        functionInfo.body = performParsingFunctionBody();

        // When a generator has a "use strict" directive, a generator function wrapping it should be strict mode.
        if  (generatorBodyScope->strictMode())
            functionScope->setStrictMode();

        semanticFailIfTrue(generatorBodyScope->hasDirectSuper(), "Cannot call super() outside of a class constructor");
        if (generatorBodyScope->needsSuperBinding())
            semanticFailIfTrue(expectedSuperBinding == SuperBinding::NotNeeded, "super can only be used in a method of a derived class");

        popScope(generatorBodyScope, TreeBuilder::NeedsFreeVariableInfo);
    } else
        functionInfo.body = performParsingFunctionBody();
    
    restoreParserState(oldState);
    failIfFalse(functionInfo.body, "Cannot parse the body of this ", stringForFunctionMode(mode));
    context.setEndOffset(functionInfo.body, m_lexer->currentOffset());
    if (functionScope->strictMode() && functionInfo.name) {
        RELEASE_ASSERT(mode == SourceParseMode::NormalFunctionMode || mode == SourceParseMode::MethodMode || mode == SourceParseMode::ArrowFunctionMode || mode == SourceParseMode::GeneratorBodyMode || mode == SourceParseMode::GeneratorWrapperFunctionMode);
        semanticFailIfTrue(m_vm->propertyNames->arguments == *functionInfo.name, "'", functionInfo.name->impl(), "' is not a valid function name in strict mode");
        semanticFailIfTrue(m_vm->propertyNames->eval == *functionInfo.name, "'", functionInfo.name->impl(), "' is not a valid function name in strict mode");
    }
    // It unncecessary to check of using super during reparsing one more time. Also it can lead to syntax error
    // in case of arrow function becuase during reparsing we don't know that parse arrow function
    // inside of the constructor or method
    if (!m_lexer->isReparsingFunction()) {
        if (functionScope->hasDirectSuper()) {
            ConstructorKind functionConstructorKind = functionBodyType == StandardFunctionBodyBlock
                ? constructorKind
                : closestParentNonArrowFunctionNonLexicalScope()->constructorKind();
            semanticFailIfTrue(functionConstructorKind == ConstructorKind::None, "Cannot call super() outside of a class constructor");
            semanticFailIfTrue(functionConstructorKind != ConstructorKind::Derived, "Cannot call super() in a base class constructor");
        }
        if (functionScope->needsSuperBinding()) {
            SuperBinding functionSuperBinding = functionBodyType == StandardFunctionBodyBlock
                ? expectedSuperBinding
                : closestParentNonArrowFunctionNonLexicalScope()->expectedSuperBinding();
            semanticFailIfTrue(functionSuperBinding == SuperBinding::NotNeeded, "super can only be used in a method of a derived class");
        }
    }

    JSTokenLocation location = JSTokenLocation(m_token.m_location);
    functionInfo.endOffset = m_token.m_data.offset;
    
    if (functionBodyType == ArrowFunctionBodyExpression) {
        location = locationBeforeLastToken();
        functionInfo.endOffset = location.endOffset;
    }
    
    // Cache the tokenizer state and the function scope the first time the function is parsed.
    // Any future reparsing can then skip the function.
    // For arrow function is 8 = x=>x + 4 symbols;
    // For ordinary function is 16  = function(){} + 4 symbols
    const int minimumFunctionLengthToCache = functionBodyType == StandardFunctionBodyBlock ? 16 : 8;
    std::unique_ptr<SourceProviderCacheItem> newInfo;
    int functionLength = functionInfo.endOffset - functionInfo.startOffset;
    if (TreeBuilder::CanUseFunctionCache && m_functionCache && functionLength > minimumFunctionLengthToCache) {
        SourceProviderCacheItemCreationParameters parameters;
        parameters.endFunctionOffset = functionInfo.endOffset;
        parameters.functionNameStart = functionNameStart;
        parameters.lastTockenLine = location.line;
        parameters.lastTockenStartOffset = location.startOffset;
        parameters.lastTockenEndOffset = location.endOffset;
        parameters.lastTockenLineStartOffset = location.lineStartOffset;
        parameters.parameterCount = functionInfo.parameterCount;
        if (functionBodyType == ArrowFunctionBodyExpression) {
            parameters.isBodyArrowExpression = true;
            parameters.tokenType = m_token.m_type;
        }
        functionScope->fillParametersForSourceProviderCache(parameters);
        newInfo = SourceProviderCacheItem::create(parameters);
    }
    
    popScope(functionScope, TreeBuilder::NeedsFreeVariableInfo);
    
    if (functionBodyType == ArrowFunctionBodyExpression)
        failIfFalse(isEndOfArrowFunction(), "Expected the closing ';' ',' ']' ')' '}', line terminator or EOF after arrow function");
    else {
        matchOrFail(CLOSEBRACE, "Expected a closing '}' after a ", stringForFunctionMode(mode), " body");
        next();
    }
    
    if (newInfo)
        m_functionCache->add(functionInfo.startOffset, WTFMove(newInfo));
    
    functionInfo.endLine = m_lastTokenEndPosition.line;
    return true;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseFunctionDeclaration(TreeBuilder& context, ExportType exportType)
{
    ASSERT(match(FUNCTION));
    JSTokenLocation location(tokenLocation());
    unsigned functionKeywordStart = tokenStart();
    next();
    ParserFunctionInfo<TreeBuilder> functionInfo;
    SourceParseMode parseMode = SourceParseMode::NormalFunctionMode;
#if ENABLE(ES6_GENERATORS)
    if (consume(TIMES))
        parseMode = SourceParseMode::GeneratorWrapperFunctionMode;
#endif
    failIfFalse((parseFunctionInfo(context, FunctionNeedsName, parseMode, true, ConstructorKind::None, SuperBinding::NotNeeded, functionKeywordStart, functionInfo, FunctionDefinitionType::Declaration)), "Cannot parse this function");
    failIfFalse(functionInfo.name, "Function statements must have a name");

    DeclarationResultMask declarationResult = declareVariable(functionInfo.name);
    failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot declare a function named '", functionInfo.name->impl(), "' in strict mode");
    if (declarationResult & DeclarationResult::InvalidDuplicateDeclaration)
        internalFailWithMessage(false, "Cannot declare a function that shadows a let/const/class variable '", functionInfo.name->impl(), "' in strict mode");
    if (exportType == ExportType::Exported) {
        semanticFailIfFalse(exportName(*functionInfo.name), "Cannot export a duplicate function name: '", functionInfo.name->impl(), "'");
        currentScope()->moduleScopeData().exportBinding(*functionInfo.name);
    }
    return context.createFuncDeclStatement(location, functionInfo);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseClassDeclaration(TreeBuilder& context, ExportType exportType)
{
    ASSERT(match(CLASSTOKEN));
    JSTokenLocation location(tokenLocation());
    JSTextPosition classStart = tokenStartPosition();
    unsigned classStartLine = tokenLine();

    ParserClassInfo<TreeBuilder> info;
    TreeClassExpression classExpr = parseClass(context, FunctionNeedsName, info);
    failIfFalse(classExpr, "Failed to parse class");

    DeclarationResultMask declarationResult = declareVariable(info.className, DeclarationType::LetDeclaration);
    if (declarationResult & DeclarationResult::InvalidDuplicateDeclaration)
        internalFailWithMessage(false, "Cannot declare a class twice: '", info.className->impl(), "'");
    if (exportType == ExportType::Exported) {
        semanticFailIfFalse(exportName(*info.className), "Cannot export a duplicate class name: '", info.className->impl(), "'");
        currentScope()->moduleScopeData().exportBinding(*info.className);
    }

    JSTextPosition classEnd = lastTokenEndPosition();
    unsigned classEndLine = tokenLine();

    return context.createClassDeclStatement(location, classExpr, classStart, classEnd, classStartLine, classEndLine);
}

template <typename LexerType>
template <class TreeBuilder> TreeClassExpression Parser<LexerType>::parseClass(TreeBuilder& context, FunctionRequirements requirements, ParserClassInfo<TreeBuilder>& info)
{
    ASSERT(match(CLASSTOKEN));
    JSTokenLocation location(tokenLocation());
    next();

    AutoPopScopeRef classScope(this, pushScope());
    classScope->setIsLexicalScope();
    classScope->preventVarDeclarations();
    classScope->setStrictMode();

    const Identifier* className = nullptr;
    if (match(IDENT)) {
        className = m_token.m_data.ident;
        info.className = className;
        next();
        failIfTrue(classScope->declareLexicalVariable(className, true) & DeclarationResult::InvalidStrictMode, "'", className->impl(), "' is not a valid class name");
    } else if (requirements == FunctionNeedsName) {
        if (match(OPENBRACE))
            semanticFail("Class statements must have a name");
        semanticFailureDueToKeyword("class name");
        failDueToUnexpectedToken();
    } else
        className = &m_vm->propertyNames->nullIdentifier;
    ASSERT(className);

    TreeExpression parentClass = 0;
    if (consume(EXTENDS)) {
        parentClass = parseMemberExpression(context);
        failIfFalse(parentClass, "Cannot parse the parent class name");
    }
    const ConstructorKind constructorKind = parentClass ? ConstructorKind::Derived : ConstructorKind::Base;

    consumeOrFail(OPENBRACE, "Expected opening '{' at the start of a class body");

    TreeExpression constructor = 0;
    TreePropertyList staticMethods = 0;
    TreePropertyList instanceMethods = 0;
    TreePropertyList instanceMethodsTail = 0;
    TreePropertyList staticMethodsTail = 0;
    while (!match(CLOSEBRACE)) {
        if (match(SEMICOLON)) {
            next();
            continue;
        }

        JSTokenLocation methodLocation(tokenLocation());
        unsigned methodStart = tokenStart();

        // For backwards compatibility, "static" is a non-reserved keyword in non-strict mode.
        bool isStaticMethod = false;
        if (match(RESERVED_IF_STRICT) && *m_token.m_data.ident == m_vm->propertyNames->staticKeyword) {
            SavePoint savePoint = createSavePoint();
            next();
            if (match(OPENPAREN)) {
                // Reparse "static()" as a method named "static".
                restoreSavePoint(savePoint);
            } else
                isStaticMethod = true;
        }

        // FIXME: Figure out a way to share more code with parseProperty.
        const CommonIdentifiers& propertyNames = *m_vm->propertyNames;
        const Identifier* ident = &propertyNames.nullIdentifier;
        TreeExpression computedPropertyName = 0;
        bool isGetter = false;
        bool isSetter = false;
        bool isGenerator = false;
#if ENABLE(ES6_GENERATORS)
        if (consume(TIMES))
            isGenerator = true;
#endif
        switch (m_token.m_type) {
        namedKeyword:
        case STRING:
            ident = m_token.m_data.ident;
            ASSERT(ident);
            next();
            break;
        case IDENT:
            ident = m_token.m_data.ident;
            ASSERT(ident);
            next();
            if (!isGenerator && (matchIdentifierOrKeyword() || match(STRING) || match(DOUBLE) || match(INTEGER) || match(OPENBRACKET))) {
                isGetter = *ident == propertyNames.get;
                isSetter = *ident == propertyNames.set;
            }
            break;
        case DOUBLE:
        case INTEGER:
            ident = &m_parserArena.identifierArena().makeNumericIdentifier(const_cast<VM*>(m_vm), m_token.m_data.doubleValue);
            ASSERT(ident);
            next();
            break;
        case OPENBRACKET:
            next();
            computedPropertyName = parseAssignmentExpression(context);
            failIfFalse(computedPropertyName, "Cannot parse computed property name");
            handleProductionOrFail(CLOSEBRACKET, "]", "end", "computed property name");
            break;
        default:
            if (m_token.m_type & KeywordTokenFlag)
                goto namedKeyword;
            failDueToUnexpectedToken();
        }

        TreeProperty property;
        const bool alwaysStrictInsideClass = true;
        if (isGetter || isSetter) {
            property = parseGetterSetter(context, alwaysStrictInsideClass, isGetter ? PropertyNode::Getter : PropertyNode::Setter, methodStart,
                ConstructorKind::None, SuperBinding::Needed);
            failIfFalse(property, "Cannot parse this method");
        } else {
            ParserFunctionInfo<TreeBuilder> methodInfo;
            bool isConstructor = !isStaticMethod && *ident == propertyNames.constructor;
            SourceParseMode parseMode = SourceParseMode::MethodMode;
            if (isGenerator) {
                isConstructor = false;
                parseMode = SourceParseMode::GeneratorWrapperFunctionMode;
                semanticFailIfTrue(*ident == m_vm->propertyNames->prototype, "Cannot declare a generator named 'prototype'");
                semanticFailIfTrue(*ident == m_vm->propertyNames->constructor, "Cannot declare a generator named 'constructor'");
            }
            failIfFalse((parseFunctionInfo(context, FunctionNoRequirements, parseMode, false, isConstructor ? constructorKind : ConstructorKind::None, SuperBinding::Needed, methodStart, methodInfo, FunctionDefinitionType::Method)), "Cannot parse this method");
            methodInfo.name = isConstructor ? className : ident;

            TreeExpression method = context.createFunctionExpr(methodLocation, methodInfo);
            if (isConstructor) {
                semanticFailIfTrue(constructor, "Cannot declare multiple constructors in a single class");
                constructor = method;
                continue;
            }

            // FIXME: Syntax error when super() is called
            semanticFailIfTrue(isStaticMethod && methodInfo.name && *methodInfo.name == propertyNames.prototype,
                "Cannot declare a static method named 'prototype'");
            if (computedPropertyName) {
                property = context.createProperty(computedPropertyName, method, static_cast<PropertyNode::Type>(PropertyNode::Constant | PropertyNode::Computed),
                    PropertyNode::Unknown, alwaysStrictInsideClass, SuperBinding::Needed);
            } else
                property = context.createProperty(methodInfo.name, method, PropertyNode::Constant, PropertyNode::Unknown, alwaysStrictInsideClass, SuperBinding::Needed);
        }

        TreePropertyList& tail = isStaticMethod ? staticMethodsTail : instanceMethodsTail;
        if (tail)
            tail = context.createPropertyList(methodLocation, property, tail);
        else {
            tail = context.createPropertyList(methodLocation, property);
            if (isStaticMethod)
                staticMethods = tail;
            else
                instanceMethods = tail;
        }
    }

    consumeOrFail(CLOSEBRACE, "Expected a closing '}' after a class body");

    auto classExpression = context.createClassExpr(location, *className, classScope->finalizeLexicalEnvironment(), constructor, parentClass, instanceMethods, staticMethods);
    popScope(classScope, TreeBuilder::NeedsFreeVariableInfo);
    return classExpression;
}

struct LabelInfo {
    LabelInfo(const Identifier* ident, const JSTextPosition& start, const JSTextPosition& end)
    : m_ident(ident)
    , m_start(start)
    , m_end(end)
    {
    }
    
    const Identifier* m_ident;
    JSTextPosition m_start;
    JSTextPosition m_end;
};

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseExpressionOrLabelStatement(TreeBuilder& context)
{
    
    /* Expression and Label statements are ambiguous at LL(1), so we have a
     * special case that looks for a colon as the next character in the input.
     */
    Vector<LabelInfo> labels;
    JSTokenLocation location;
    do {
        JSTextPosition start = tokenStartPosition();
        location = tokenLocation();
        if (!nextTokenIsColon()) {
            // If we hit this path we're making a expression statement, which
            // by definition can't make use of continue/break so we can just
            // ignore any labels we might have accumulated.
            TreeExpression expression = parseExpression(context);
            failIfFalse(expression, "Cannot parse expression statement");
            if (!autoSemiColon())
                failDueToUnexpectedToken();
            return context.createExprStatement(location, expression, start, m_lastTokenEndPosition.line);
        }
        const Identifier* ident = m_token.m_data.ident;
        JSTextPosition end = tokenEndPosition();
        next();
        consumeOrFail(COLON, "Labels must be followed by a ':'");
        if (!m_syntaxAlreadyValidated) {
            // This is O(N^2) over the current list of consecutive labels, but I
            // have never seen more than one label in a row in the real world.
            for (size_t i = 0; i < labels.size(); i++)
                failIfTrue(ident->impl() == labels[i].m_ident->impl(), "Attempted to redeclare the label '", ident->impl(), "'");
            failIfTrue(getLabel(ident), "Cannot find scope for the label '", ident->impl(), "'");
            labels.append(LabelInfo(ident, start, end));
        }
    } while (matchSpecIdentifier());
    bool isLoop = false;
    switch (m_token.m_type) {
    case FOR:
    case WHILE:
    case DO:
        isLoop = true;
        break;
        
    default:
        break;
    }
    const Identifier* unused = 0;
    ScopeRef labelScope = currentScope();
    if (!m_syntaxAlreadyValidated) {
        for (size_t i = 0; i < labels.size(); i++)
            pushLabel(labels[i].m_ident, isLoop);
    }
    TreeStatement statement = parseStatement(context, unused);
    if (!m_syntaxAlreadyValidated) {
        for (size_t i = 0; i < labels.size(); i++)
            popLabel(labelScope);
    }
    failIfFalse(statement, "Cannot parse statement");
    for (size_t i = 0; i < labels.size(); i++) {
        const LabelInfo& info = labels[labels.size() - i - 1];
        statement = context.createLabelStatement(location, info.m_ident, statement, info.m_start, info.m_end);
    }
    return statement;
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseExpressionStatement(TreeBuilder& context)
{
    switch (m_token.m_type) {
    // Consult: http://www.ecma-international.org/ecma-262/6.0/index.html#sec-expression-statement
    // The ES6 spec mandates that we should fail from FUNCTION token here. We handle this case 
    // in parseStatement() which is the only caller of parseExpressionStatement().
    // We actually allow FUNCTION in situations where it should not be allowed unless we're in strict mode.
    case CLASSTOKEN:
        failWithMessage("'class' declaration is not directly within a block statement");
        break;
    default:
        // FIXME: when implementing 'let' we should fail when we see the token sequence "let [".
        // https://bugs.webkit.org/show_bug.cgi?id=142944
        break;
    }
    JSTextPosition start = tokenStartPosition();
    JSTokenLocation location(tokenLocation());
    TreeExpression expression = parseExpression(context);
    failIfFalse(expression, "Cannot parse expression statement");
    failIfFalse(autoSemiColon(), "Parse error");
    return context.createExprStatement(location, expression, start, m_lastTokenEndPosition.line);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseIfStatement(TreeBuilder& context)
{
    ASSERT(match(IF));
    JSTokenLocation ifLocation(tokenLocation());
    int start = tokenLine();
    next();
    handleProductionOrFail(OPENPAREN, "(", "start", "'if' condition");

    TreeExpression condition = parseExpression(context);
    failIfFalse(condition, "Expected a expression as the condition for an if statement");
    int end = tokenLine();
    handleProductionOrFail(CLOSEPAREN, ")", "end", "'if' condition");

    const Identifier* unused = 0;
    TreeStatement trueBlock = parseStatement(context, unused);
    failIfFalse(trueBlock, "Expected a statement as the body of an if block");

    if (!match(ELSE))
        return context.createIfStatement(ifLocation, condition, trueBlock, 0, start, end);

    Vector<TreeExpression> exprStack;
    Vector<std::pair<int, int>> posStack;
    Vector<JSTokenLocation> tokenLocationStack;
    Vector<TreeStatement> statementStack;
    bool trailingElse = false;
    do {
        JSTokenLocation tempLocation = tokenLocation();
        next();
        if (!match(IF)) {
            const Identifier* unused = 0;
            TreeStatement block = parseStatement(context, unused);
            failIfFalse(block, "Expected a statement as the body of an else block");
            statementStack.append(block);
            trailingElse = true;
            break;
        }
        int innerStart = tokenLine();
        next();
        
        handleProductionOrFail(OPENPAREN, "(", "start", "'if' condition");

        TreeExpression innerCondition = parseExpression(context);
        failIfFalse(innerCondition, "Expected a expression as the condition for an if statement");
        int innerEnd = tokenLine();
        handleProductionOrFail(CLOSEPAREN, ")", "end", "'if' condition");
        const Identifier* unused = 0;
        TreeStatement innerTrueBlock = parseStatement(context, unused);
        failIfFalse(innerTrueBlock, "Expected a statement as the body of an if block");
        tokenLocationStack.append(tempLocation);
        exprStack.append(innerCondition);
        posStack.append(std::make_pair(innerStart, innerEnd));
        statementStack.append(innerTrueBlock);
    } while (match(ELSE));

    if (!trailingElse) {
        TreeExpression condition = exprStack.last();
        exprStack.removeLast();
        TreeStatement trueBlock = statementStack.last();
        statementStack.removeLast();
        std::pair<int, int> pos = posStack.last();
        posStack.removeLast();
        JSTokenLocation elseLocation = tokenLocationStack.last();
        tokenLocationStack.removeLast();
        TreeStatement ifStatement = context.createIfStatement(elseLocation, condition, trueBlock, 0, pos.first, pos.second);
        context.setEndOffset(ifStatement, context.endOffset(trueBlock));
        statementStack.append(ifStatement);
    }

    while (!exprStack.isEmpty()) {
        TreeExpression condition = exprStack.last();
        exprStack.removeLast();
        TreeStatement falseBlock = statementStack.last();
        statementStack.removeLast();
        TreeStatement trueBlock = statementStack.last();
        statementStack.removeLast();
        std::pair<int, int> pos = posStack.last();
        posStack.removeLast();
        JSTokenLocation elseLocation = tokenLocationStack.last();
        tokenLocationStack.removeLast();
        TreeStatement ifStatement = context.createIfStatement(elseLocation, condition, trueBlock, falseBlock, pos.first, pos.second);
        context.setEndOffset(ifStatement, context.endOffset(falseBlock));
        statementStack.append(ifStatement);
    }

    return context.createIfStatement(ifLocation, condition, trueBlock, statementStack.last(), start, end);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::ModuleName Parser<LexerType>::parseModuleName(TreeBuilder& context)
{
    // ModuleName (ModuleSpecifier in the spec) represents the module name imported by the script.
    // http://www.ecma-international.org/ecma-262/6.0/#sec-imports
    // http://www.ecma-international.org/ecma-262/6.0/#sec-exports
    JSTokenLocation specifierLocation(tokenLocation());
    failIfFalse(match(STRING), "Imported modules names must be string literals");
    const Identifier* moduleName = m_token.m_data.ident;
    next();
    return context.createModuleName(specifierLocation, *moduleName);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::ImportSpecifier Parser<LexerType>::parseImportClauseItem(TreeBuilder& context, ImportSpecifierType specifierType)
{
    // Produced node is the item of the ImportClause.
    // That is the ImportSpecifier, ImportedDefaultBinding or NameSpaceImport.
    // http://www.ecma-international.org/ecma-262/6.0/#sec-imports
    JSTokenLocation specifierLocation(tokenLocation());
    JSToken localNameToken;
    const Identifier* importedName = nullptr;
    const Identifier* localName = nullptr;

    switch (specifierType) {
    case ImportSpecifierType::NamespaceImport: {
        // NameSpaceImport :
        // * as ImportedBinding
        // e.g.
        //     * as namespace
        ASSERT(match(TIMES));
        importedName = &m_vm->propertyNames->timesIdentifier;
        next();

        failIfFalse(matchContextualKeyword(m_vm->propertyNames->as), "Expected 'as' before imported binding name");
        next();

        matchOrFail(IDENT, "Expected a variable name for the import declaration");
        localNameToken = m_token;
        localName = m_token.m_data.ident;
        next();
        break;
    }

    case ImportSpecifierType::NamedImport: {
        // ImportSpecifier :
        // ImportedBinding
        // IdentifierName as ImportedBinding
        // e.g.
        //     A
        //     A as B
        ASSERT(matchIdentifierOrKeyword());
        localNameToken = m_token;
        localName = m_token.m_data.ident;
        importedName = localName;
        next();

        if (matchContextualKeyword(m_vm->propertyNames->as)) {
            next();
            matchOrFail(IDENT, "Expected a variable name for the import declaration");
            localNameToken = m_token;
            localName = m_token.m_data.ident;
            next();
        }
        break;
    }

    case ImportSpecifierType::DefaultImport: {
        // ImportedDefaultBinding :
        // ImportedBinding
        ASSERT(match(IDENT));
        localNameToken = m_token;
        localName = m_token.m_data.ident;
        importedName = &m_vm->propertyNames->defaultKeyword;
        next();
        break;
    }
    }

    semanticFailIfTrue(localNameToken.m_type & KeywordTokenFlag, "Cannot use keyword as imported binding name");
    DeclarationResultMask declarationResult = declareVariable(localName, DeclarationType::ConstDeclaration, (specifierType == ImportSpecifierType::NamespaceImport) ? DeclarationImportType::ImportedNamespace : DeclarationImportType::Imported);
    if (declarationResult != DeclarationResult::Valid) {
        failIfTrueIfStrict(declarationResult & DeclarationResult::InvalidStrictMode, "Cannot declare an imported binding named ", localName->impl(), " in strict mode");
        if (declarationResult & DeclarationResult::InvalidDuplicateDeclaration)
            internalFailWithMessage(false, "Cannot declare an imported binding name twice: '", localName->impl(), "'");
    }

    return context.createImportSpecifier(specifierLocation, *importedName, *localName);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseImportDeclaration(TreeBuilder& context)
{
    // http://www.ecma-international.org/ecma-262/6.0/#sec-imports
    ASSERT(match(IMPORT));
    JSTokenLocation importLocation(tokenLocation());
    next();

    auto specifierList = context.createImportSpecifierList();

    if (match(STRING)) {
        // import ModuleSpecifier ;
        auto moduleName = parseModuleName(context);
        failIfFalse(moduleName, "Cannot parse the module name");
        failIfFalse(autoSemiColon(), "Expected a ';' following a targeted import declaration");
        return context.createImportDeclaration(importLocation, specifierList, moduleName);
    }

    bool isFinishedParsingImport = false;
    if (match(IDENT)) {
        // ImportedDefaultBinding :
        // ImportedBinding
        auto specifier = parseImportClauseItem(context, ImportSpecifierType::DefaultImport);
        failIfFalse(specifier, "Cannot parse the default import");
        context.appendImportSpecifier(specifierList, specifier);
        if (match(COMMA))
            next();
        else
            isFinishedParsingImport = true;
    }

    if (!isFinishedParsingImport) {
        if (match(TIMES)) {
            // import NameSpaceImport FromClause ;
            auto specifier = parseImportClauseItem(context, ImportSpecifierType::NamespaceImport);
            failIfFalse(specifier, "Cannot parse the namespace import");
            context.appendImportSpecifier(specifierList, specifier);
        } else if (match(OPENBRACE)) {
            // NamedImports :
            // { }
            // { ImportsList }
            // { ImportsList , }
            next();

            while (!match(CLOSEBRACE)) {
                failIfFalse(matchIdentifierOrKeyword(), "Expected an imported name for the import declaration");
                auto specifier = parseImportClauseItem(context, ImportSpecifierType::NamedImport);
                failIfFalse(specifier, "Cannot parse the named import");
                context.appendImportSpecifier(specifierList, specifier);
                if (!consume(COMMA))
                    break;
            }
            handleProductionOrFail(CLOSEBRACE, "}", "end", "import list");
        } else
            failWithMessage("Expected namespace import or import list");
    }

    // FromClause :
    // from ModuleSpecifier

    failIfFalse(matchContextualKeyword(m_vm->propertyNames->from), "Expected 'from' before imported module name");
    next();

    auto moduleName = parseModuleName(context);
    failIfFalse(moduleName, "Cannot parse the module name");
    failIfFalse(autoSemiColon(), "Expected a ';' following a targeted import declaration");

    return context.createImportDeclaration(importLocation, specifierList, moduleName);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::ExportSpecifier Parser<LexerType>::parseExportSpecifier(TreeBuilder& context, Vector<const Identifier*>& maybeLocalNames, bool& hasKeywordForLocalBindings)
{
    // ExportSpecifier :
    // IdentifierName
    // IdentifierName as IdentifierName
    // http://www.ecma-international.org/ecma-262/6.0/#sec-exports
    ASSERT(matchIdentifierOrKeyword());
    JSTokenLocation specifierLocation(tokenLocation());
    if (m_token.m_type & KeywordTokenFlag)
        hasKeywordForLocalBindings = true;
    const Identifier* localName = m_token.m_data.ident;
    const Identifier* exportedName = localName;
    next();

    if (matchContextualKeyword(m_vm->propertyNames->as)) {
        next();
        failIfFalse(matchIdentifierOrKeyword(), "Expected an exported name for the export declaration");
        exportedName = m_token.m_data.ident;
        next();
    }

    semanticFailIfFalse(exportName(*exportedName), "Cannot export a duplicate name '", exportedName->impl(), "'");
    maybeLocalNames.append(localName);
    return context.createExportSpecifier(specifierLocation, *localName, *exportedName);
}

template <typename LexerType>
template <class TreeBuilder> TreeStatement Parser<LexerType>::parseExportDeclaration(TreeBuilder& context)
{
    // http://www.ecma-international.org/ecma-262/6.0/#sec-exports
    ASSERT(match(EXPORT));
    JSTokenLocation exportLocation(tokenLocation());
    next();

    switch (m_token.m_type) {
    case TIMES: {
        // export * FromClause ;
        next();

        failIfFalse(matchContextualKeyword(m_vm->propertyNames->from), "Expected 'from' before exported module name");
        next();
        auto moduleName = parseModuleName(context);
        failIfFalse(moduleName, "Cannot parse the 'from' clause");
        failIfFalse(autoSemiColon(), "Expected a ';' following a targeted export declaration");

        return context.createExportAllDeclaration(exportLocation, moduleName);
    }

    case DEFAULT: {
        // export default HoistableDeclaration[Default]
        // export default ClassDeclaration[Default]
        // export default [lookahead not-in {function, class}] AssignmentExpression[In] ;

        next();

        TreeStatement result = 0;
        bool isFunctionOrClassDeclaration = false;
        const Identifier* localName = nullptr;
        SavePoint savePoint = createSavePoint();

        bool startsWithFunction = match(FUNCTION);
        if (startsWithFunction
#if ENABLE(ES6_CLASS_SYNTAX)
                || match(CLASSTOKEN)
#endif
                ) {
            isFunctionOrClassDeclaration = true;
            next();

#if ENABLE(ES6_GENERATORS)
            // ES6 Generators
            if (startsWithFunction && match(TIMES))
                next();
#endif
            if (match(IDENT))
                localName = m_token.m_data.ident;
            restoreSavePoint(savePoint);
        }

        if (localName) {
            if (match(FUNCTION))
                result = parseFunctionDeclaration(context);
#if ENABLE(ES6_CLASS_SYNTAX)
            else {
                ASSERT(match(CLASSTOKEN));
                result = parseClassDeclaration(context);
            }
#endif
        } else {
            // export default expr;
            //
            // It should be treated as the same to the following.
            //
            // const *default* = expr;
            // export { *default* as default }
            //
            // In the above example, *default* is the invisible variable to the users.
            // We use the private symbol to represent the name of this variable.
            JSTokenLocation location(tokenLocation());
            JSTextPosition start = tokenStartPosition();
            TreeExpression expression = parseAssignmentExpression(context);
            failIfFalse(expression, "Cannot parse expression");

            DeclarationResultMask declarationResult = declareVariable(&m_vm->propertyNames->starDefaultPrivateName, DeclarationType::ConstDeclaration);
            if (declarationResult & DeclarationResult::InvalidDuplicateDeclaration)
                internalFailWithMessage(false, "Only one 'default' export is allowed");

            TreeExpression assignment = context.createAssignResolve(location, m_vm->propertyNames->starDefaultPrivateName, expression, start, start, tokenEndPosition(), AssignmentContext::ConstDeclarationStatement);
            result = context.createExprStatement(location, assignment, start, tokenEndPosition());
            if (!isFunctionOrClassDeclaration)
                failIfFalse(autoSemiColon(), "Expected a ';' following a targeted export declaration");
            localName = &m_vm->propertyNames->starDefaultPrivateName;
        }
        failIfFalse(result, "Cannot parse the declaration");

        semanticFailIfFalse(exportName(m_vm->propertyNames->defaultKeyword), "Only one 'default' export is allowed");
        currentScope()->moduleScopeData().exportBinding(*localName);
        return context.createExportDefaultDeclaration(exportLocation, result, *localName);
    }

    case OPENBRACE: {
        // export ExportClause FromClause ;
        // export ExportClause ;
        //
        // ExportClause :
        // { }
        // { ExportsList }
        // { ExportsList , }
        //
        // ExportsList :
        // ExportSpecifier
        // ExportsList , ExportSpecifier

        next();

        auto specifierList = context.createExportSpecifierList();
        Vector<const Identifier*> maybeLocalNames;

        bool hasKeywordForLocalBindings = false;
        while (!match(CLOSEBRACE)) {
            failIfFalse(matchIdentifierOrKeyword(), "Expected a variable name for the export declaration");
            auto specifier = parseExportSpecifier(context, maybeLocalNames, hasKeywordForLocalBindings);
            failIfFalse(specifier, "Cannot parse the named export");
            context.appendExportSpecifier(specifierList, specifier);
            if (!consume(COMMA))
                break;
        }
        handleProductionOrFail(CLOSEBRACE, "}", "end", "export list");

        typename TreeBuilder::ModuleName moduleName = 0;
        if (matchContextualKeyword(m_vm->propertyNames->from)) {
            next();
            moduleName = parseModuleName(context);
            failIfFalse(moduleName, "Cannot parse the 'from' clause");
        }
        failIfFalse(autoSemiColon(), "Expected a ';' following a targeted export declaration");

        if (!moduleName) {
            semanticFailIfTrue(hasKeywordForLocalBindings, "Cannot use keyword as exported variable name");
            // Since this export declaration does not have module specifier part, it exports the local bindings.
            // While the export declaration with module specifier does not have any effect on the current module's scope,
            // the export named declaration without module specifier references the the local binding names.
            // For example,
            //   export { A, B, C as D } from "mod"
            // does not have effect on the current module's scope. But,
            //   export { A, B, C as D }
            // will reference the current module's bindings.
            for (const Identifier* localName : maybeLocalNames)
                currentScope()->moduleScopeData().exportBinding(*localName);
        }

        return context.createExportNamedDeclaration(exportLocation, specifierList, moduleName);
    }

    default: {
        // export VariableStatement
        // export Declaration
        TreeStatement result = 0;
        switch (m_token.m_type) {
        case VAR:
            result = parseVariableDeclaration(context, DeclarationType::VarDeclaration, ExportType::Exported);
            break;

        case CONSTTOKEN:
            result = parseVariableDeclaration(context, DeclarationType::ConstDeclaration, ExportType::Exported);
            break;

        case LET:
            result = parseVariableDeclaration(context, DeclarationType::LetDeclaration, ExportType::Exported);
            break;

        case FUNCTION:
            result = parseFunctionDeclaration(context, ExportType::Exported);
            break;

#if ENABLE(ES6_CLASS_SYNTAX)
        case CLASSTOKEN:
            result = parseClassDeclaration(context, ExportType::Exported);
            break;
#endif

        default:
            failWithMessage("Expected either a declaration or a variable statement");
            break;
        }
        failIfFalse(result, "Cannot parse the declaration");
        return context.createExportLocalDeclaration(exportLocation, result);
    }
    }

    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseExpression(TreeBuilder& context)
{
    failIfStackOverflow();
    JSTokenLocation location(tokenLocation());
    TreeExpression node = parseAssignmentExpression(context);
    failIfFalse(node, "Cannot parse expression");
    context.setEndOffset(node, m_lastTokenEndPosition.offset);
    if (!match(COMMA))
        return node;
    next();
    m_parserState.nonTrivialExpressionCount++;
    m_parserState.nonLHSCount++;
    TreeExpression right = parseAssignmentExpression(context);
    failIfFalse(right, "Cannot parse expression in a comma expression");
    context.setEndOffset(right, m_lastTokenEndPosition.offset);
    typename TreeBuilder::Comma head = context.createCommaExpr(location, node);
    typename TreeBuilder::Comma tail = context.appendToCommaExpr(location, head, head, right);
    while (match(COMMA)) {
        next(TreeBuilder::DontBuildStrings);
        right = parseAssignmentExpression(context);
        failIfFalse(right, "Cannot parse expression in a comma expression");
        context.setEndOffset(right, m_lastTokenEndPosition.offset);
        tail = context.appendToCommaExpr(location, head, tail, right);
    }
    context.setEndOffset(head, m_lastTokenEndPosition.offset);
    return head;
}

template <typename LexerType>
template <typename TreeBuilder> TreeExpression Parser<LexerType>::parseAssignmentExpressionOrPropagateErrorClass(TreeBuilder& context)
{
    ExpressionErrorClassifier classifier(this);
    auto assignment = parseAssignmentExpression(context, classifier);
    if (!assignment)
        classifier.propagateExpressionErrorClass();
    return assignment;
}

template <typename LexerType>
template <typename TreeBuilder> TreeExpression Parser<LexerType>::parseAssignmentExpression(TreeBuilder& context)
{
    ExpressionErrorClassifier classifier(this);
    return parseAssignmentExpression(context, classifier);
}
    
template <typename LexerType>
template <typename TreeBuilder> TreeExpression Parser<LexerType>::parseAssignmentExpression(TreeBuilder& context, ExpressionErrorClassifier& classifier)
{
    ASSERT(!hasError());
    
    failIfStackOverflow();
    JSTextPosition start = tokenStartPosition();
    JSTokenLocation location(tokenLocation());
    int initialAssignmentCount = m_parserState.assignmentCount;
    int initialNonLHSCount = m_parserState.nonLHSCount;
    bool maybeAssignmentPattern = match(OPENBRACE) || match(OPENBRACKET);
    SavePoint savePoint = createSavePoint();

#if ENABLE(ES6_GENERATORS)
    if (match(YIELD) && !isYIELDMaskedAsIDENT(currentScope()->isGenerator()))
        return parseYieldExpression(context);
#endif

#if ENABLE(ES6_ARROWFUNCTION_SYNTAX)
    if (isArrowFunctionParameters())
        return parseArrowFunctionExpression(context);
#endif
    
    TreeExpression lhs = parseConditionalExpression(context);

    if (!lhs && (!maybeAssignmentPattern || !classifier.indicatesPossiblePattern()))
        propagateError();

    if (maybeAssignmentPattern && (!lhs || (context.isObjectOrArrayLiteral(lhs) && match(EQUAL)))) {
        String expressionError = m_errorMessage;
        SavePoint expressionErrorLocation = createSavePointForError();
        restoreSavePoint(savePoint);
        auto pattern = tryParseDestructuringPatternExpression(context, AssignmentContext::AssignmentExpression);
        if (classifier.indicatesPossiblePattern() && (!pattern || !match(EQUAL)))
            restoreSavePointAndFail(expressionErrorLocation, expressionError);
        failIfFalse(pattern, "Cannot parse assignment pattern");
        consumeOrFail(EQUAL, "Expected '=' following assignment pattern");
        auto rhs = parseAssignmentExpression(context);
        if (!rhs)
            propagateError();
        return context.createDestructuringAssignment(location, pattern, rhs);
    }

    failIfFalse(lhs, "Cannot parse expression");
    if (initialNonLHSCount != m_parserState.nonLHSCount) {
        if (m_token.m_type >= EQUAL && m_token.m_type <= OREQUAL)
            semanticFail("Left hand side of operator '", getToken(), "' must be a reference");

        return lhs;
    }
    
    int assignmentStack = 0;
    Operator op;
    bool hadAssignment = false;
    while (true) {
        switch (m_token.m_type) {
        case EQUAL: op = OpEqual; break;
        case PLUSEQUAL: op = OpPlusEq; break;
        case MINUSEQUAL: op = OpMinusEq; break;
        case MULTEQUAL: op = OpMultEq; break;
        case DIVEQUAL: op = OpDivEq; break;
        case LSHIFTEQUAL: op = OpLShift; break;
        case RSHIFTEQUAL: op = OpRShift; break;
        case URSHIFTEQUAL: op = OpURShift; break;
        case ANDEQUAL: op = OpAndEq; break;
        case XOREQUAL: op = OpXOrEq; break;
        case OREQUAL: op = OpOrEq; break;
        case MODEQUAL: op = OpModEq; break;
        default:
            goto end;
        }
        m_parserState.nonTrivialExpressionCount++;
        hadAssignment = true;
        context.assignmentStackAppend(assignmentStack, lhs, start, tokenStartPosition(), m_parserState.assignmentCount, op);
        start = tokenStartPosition();
        m_parserState.assignmentCount++;
        next(TreeBuilder::DontBuildStrings);
        if (strictMode() && m_parserState.lastIdentifier && context.isResolve(lhs)) {
            failIfTrueIfStrict(m_vm->propertyNames->eval == *m_parserState.lastIdentifier, "Cannot modify 'eval' in strict mode");
            failIfTrueIfStrict(m_vm->propertyNames->arguments == *m_parserState.lastIdentifier, "Cannot modify 'arguments' in strict mode");
            declareWrite(m_parserState.lastIdentifier);
            m_parserState.lastIdentifier = 0;
        }
        lhs = parseAssignmentExpression(context);
        failIfFalse(lhs, "Cannot parse the right hand side of an assignment expression");
        if (initialNonLHSCount != m_parserState.nonLHSCount) {
            if (m_token.m_type >= EQUAL && m_token.m_type <= OREQUAL)
                semanticFail("Left hand side of operator '", getToken(), "' must be a reference");
            break;
        }
    }
end:
    if (hadAssignment)
        m_parserState.nonLHSCount++;
    
    if (!TreeBuilder::CreatesAST)
        return lhs;
    
    while (assignmentStack)
        lhs = context.createAssignment(location, assignmentStack, lhs, initialAssignmentCount, m_parserState.assignmentCount, lastTokenEndPosition());
    
    return lhs;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseYieldExpression(TreeBuilder& context)
{
    // YieldExpression[In] :
    //     yield
    //     yield [no LineTerminator here] AssignmentExpression[?In, Yield]
    //     yield [no LineTerminator here] * AssignmentExpression[?In, Yield]

    // http://ecma-international.org/ecma-262/6.0/#sec-generator-function-definitions
    failIfFalse(currentScope()->isGenerator(), "Cannot use yield expression out of generator");

    // http://ecma-international.org/ecma-262/6.0/#sec-generator-function-definitions-static-semantics-early-errors
    failIfTrue(m_parserState.functionParsePhase == FunctionParsePhase::Parameters, "Cannot use yield expression within parameters");

    JSTokenLocation location(tokenLocation());
    JSTextPosition divotStart = tokenStartPosition();
    ASSERT(match(YIELD));
    SavePoint savePoint = createSavePoint();
    next();
    if (m_lexer->prevTerminator())
        return context.createYield(location);

    bool delegate = consume(TIMES);
    JSTextPosition argumentStart = tokenStartPosition();
    TreeExpression argument = parseAssignmentExpression(context);
    if (!argument) {
        restoreSavePoint(savePoint);
        next();
        return context.createYield(location);
    }
    return context.createYield(location, argument, delegate, divotStart, argumentStart, lastTokenEndPosition());
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseConditionalExpression(TreeBuilder& context)
{
    JSTokenLocation location(tokenLocation());
    TreeExpression cond = parseBinaryExpression(context);
    failIfFalse(cond, "Cannot parse expression");
    if (!match(QUESTION))
        return cond;
    m_parserState.nonTrivialExpressionCount++;
    m_parserState.nonLHSCount++;
    next(TreeBuilder::DontBuildStrings);
    TreeExpression lhs = parseAssignmentExpression(context);
    failIfFalse(lhs, "Cannot parse left hand side of ternary operator");
    context.setEndOffset(lhs, m_lastTokenEndPosition.offset);
    consumeOrFailWithFlags(COLON, TreeBuilder::DontBuildStrings, "Expected ':' in ternary operator");
    
    TreeExpression rhs = parseAssignmentExpression(context);
    failIfFalse(rhs, "Cannot parse right hand side of ternary operator");
    context.setEndOffset(rhs, m_lastTokenEndPosition.offset);
    return context.createConditionalExpr(location, cond, lhs, rhs);
}

ALWAYS_INLINE static bool isUnaryOp(JSTokenType token)
{
    return token & UnaryOpTokenFlag;
}

template <typename LexerType>
int Parser<LexerType>::isBinaryOperator(JSTokenType token)
{
    if (m_allowsIn)
        return token & (BinaryOpTokenPrecedenceMask << BinaryOpTokenAllowsInPrecedenceAdditionalShift);
    return token & BinaryOpTokenPrecedenceMask;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseBinaryExpression(TreeBuilder& context)
{
    int operandStackDepth = 0;
    int operatorStackDepth = 0;
    typename TreeBuilder::BinaryExprContext binaryExprContext(context);
    JSTokenLocation location(tokenLocation());
    while (true) {
        JSTextPosition exprStart = tokenStartPosition();
        int initialAssignments = m_parserState.assignmentCount;
        TreeExpression current = parseUnaryExpression(context);
        failIfFalse(current, "Cannot parse expression");
        
        context.appendBinaryExpressionInfo(operandStackDepth, current, exprStart, lastTokenEndPosition(), lastTokenEndPosition(), initialAssignments != m_parserState.assignmentCount);
        int precedence = isBinaryOperator(m_token.m_type);
        if (!precedence)
            break;
        m_parserState.nonTrivialExpressionCount++;
        m_parserState.nonLHSCount++;
        int operatorToken = m_token.m_type;
        next(TreeBuilder::DontBuildStrings);
        
        while (operatorStackDepth &&  context.operatorStackHasHigherPrecedence(operatorStackDepth, precedence)) {
            ASSERT(operandStackDepth > 1);
            
            typename TreeBuilder::BinaryOperand rhs = context.getFromOperandStack(-1);
            typename TreeBuilder::BinaryOperand lhs = context.getFromOperandStack(-2);
            context.shrinkOperandStackBy(operandStackDepth, 2);
            context.appendBinaryOperation(location, operandStackDepth, operatorStackDepth, lhs, rhs);
            context.operatorStackPop(operatorStackDepth);
        }
        context.operatorStackAppend(operatorStackDepth, operatorToken, precedence);
    }
    while (operatorStackDepth) {
        ASSERT(operandStackDepth > 1);
        
        typename TreeBuilder::BinaryOperand rhs = context.getFromOperandStack(-1);
        typename TreeBuilder::BinaryOperand lhs = context.getFromOperandStack(-2);
        context.shrinkOperandStackBy(operandStackDepth, 2);
        context.appendBinaryOperation(location, operandStackDepth, operatorStackDepth, lhs, rhs);
        context.operatorStackPop(operatorStackDepth);
    }
    return context.popOperandStack(operandStackDepth);
}

template <typename LexerType>
template <class TreeBuilder> TreeProperty Parser<LexerType>::parseProperty(TreeBuilder& context, bool complete)
{
    bool wasIdent = false;
    bool isGenerator = false;
#if ENABLE(ES6_GENERATORS)
    if (consume(TIMES))
        isGenerator = true;
#endif
    switch (m_token.m_type) {
    namedProperty:
    case IDENT:
        wasIdent = true;
        FALLTHROUGH;
    case STRING: {
        const Identifier* ident = m_token.m_data.ident;
        unsigned getterOrSetterStartOffset = tokenStart();
        if (complete || (wasIdent && !isGenerator && (*ident == m_vm->propertyNames->get || *ident == m_vm->propertyNames->set)))
            nextExpectIdentifier(LexerFlagsIgnoreReservedWords);
        else
            nextExpectIdentifier(LexerFlagsIgnoreReservedWords | TreeBuilder::DontBuildKeywords);

        if (!isGenerator && match(COLON)) {
            next();
            TreeExpression node = parseAssignmentExpressionOrPropagateErrorClass(context);
            failIfFalse(node, "Cannot parse expression for property declaration");
            context.setEndOffset(node, m_lexer->currentOffset());
            return context.createProperty(ident, node, PropertyNode::Constant, PropertyNode::Unknown, complete);
        }

        if (match(OPENPAREN)) {
            auto method = parsePropertyMethod(context, ident, isGenerator);
            propagateError();
            return context.createProperty(ident, method, PropertyNode::Constant, PropertyNode::KnownDirect, complete);
        }
        failIfTrue(isGenerator, "Expected a parenthesis for argument list");

        failIfFalse(wasIdent, "Expected an identifier as property name");

        if (match(COMMA) || match(CLOSEBRACE)) {
            JSTextPosition start = tokenStartPosition();
            JSTokenLocation location(tokenLocation());
            currentScope()->useVariable(ident, m_vm->propertyNames->eval == *ident);
            TreeExpression node = context.createResolve(location, *ident, start, lastTokenEndPosition());
            return context.createProperty(ident, node, static_cast<PropertyNode::Type>(PropertyNode::Constant | PropertyNode::Shorthand), PropertyNode::KnownDirect, complete);
        }

        if (match(EQUAL)) // CoverInitializedName is exclusive to BindingPattern and AssignmentPattern
            classifyExpressionError(ErrorIndicatesPattern);

        PropertyNode::Type type;
        if (*ident == m_vm->propertyNames->get)
            type = PropertyNode::Getter;
        else if (*ident == m_vm->propertyNames->set)
            type = PropertyNode::Setter;
        else
            failWithMessage("Expected a ':' following the property name '", ident->impl(), "'");
        return parseGetterSetter(context, complete, type, getterOrSetterStartOffset);
    }
    case DOUBLE:
    case INTEGER: {
        double propertyName = m_token.m_data.doubleValue;
        next();

        if (match(OPENPAREN)) {
            const Identifier& ident = m_parserArena.identifierArena().makeNumericIdentifier(const_cast<VM*>(m_vm), propertyName);
            auto method = parsePropertyMethod(context, &ident, isGenerator);
            propagateError();
            return context.createProperty(&ident, method, PropertyNode::Constant, PropertyNode::Unknown, complete);
        }
        failIfTrue(isGenerator, "Expected a parenthesis for argument list");

        consumeOrFail(COLON, "Expected ':' after property name");
        TreeExpression node = parseAssignmentExpression(context);
        failIfFalse(node, "Cannot parse expression for property declaration");
        context.setEndOffset(node, m_lexer->currentOffset());
        return context.createProperty(const_cast<VM*>(m_vm), m_parserArena, propertyName, node, PropertyNode::Constant, PropertyNode::Unknown, complete);
    }
    case OPENBRACKET: {
        next();
        auto propertyName = parseAssignmentExpression(context);
        failIfFalse(propertyName, "Cannot parse computed property name");
        handleProductionOrFail(CLOSEBRACKET, "]", "end", "computed property name");

        if (match(OPENPAREN)) {
            auto method = parsePropertyMethod(context, &m_vm->propertyNames->nullIdentifier, isGenerator);
            propagateError();
            return context.createProperty(propertyName, method, static_cast<PropertyNode::Type>(PropertyNode::Constant | PropertyNode::Computed), PropertyNode::KnownDirect, complete);
        }
        failIfTrue(isGenerator, "Expected a parenthesis for argument list");

        consumeOrFail(COLON, "Expected ':' after property name");
        TreeExpression node = parseAssignmentExpression(context);
        failIfFalse(node, "Cannot parse expression for property declaration");
        context.setEndOffset(node, m_lexer->currentOffset());
        return context.createProperty(propertyName, node, static_cast<PropertyNode::Type>(PropertyNode::Constant | PropertyNode::Computed), PropertyNode::Unknown, complete);
    }
    default:
        failIfFalse(m_token.m_type & KeywordTokenFlag, "Expected a property name");
        goto namedProperty;
    }
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parsePropertyMethod(TreeBuilder& context, const Identifier* methodName, bool isGenerator)
{
    JSTokenLocation methodLocation(tokenLocation());
    unsigned methodStart = tokenStart();
    ParserFunctionInfo<TreeBuilder> methodInfo;
    SourceParseMode parseMode = isGenerator ? SourceParseMode::GeneratorWrapperFunctionMode : SourceParseMode::MethodMode;
    failIfFalse((parseFunctionInfo(context, FunctionNoRequirements, parseMode, false, ConstructorKind::None, SuperBinding::NotNeeded, methodStart, methodInfo, FunctionDefinitionType::Method)), "Cannot parse this method");
    methodInfo.name = methodName;
    return context.createFunctionExpr(methodLocation, methodInfo);
}

template <typename LexerType>
template <class TreeBuilder> TreeProperty Parser<LexerType>::parseGetterSetter(TreeBuilder& context, bool strict, PropertyNode::Type type, unsigned getterOrSetterStartOffset,
    ConstructorKind constructorKind, SuperBinding superBinding)
{
    const Identifier* stringPropertyName = 0;
    double numericPropertyName = 0;
    TreeExpression computedPropertyName = 0;

    JSTokenLocation location(tokenLocation());

    if (matchSpecIdentifier() || match(STRING) || m_token.m_type & KeywordTokenFlag) {
        stringPropertyName = m_token.m_data.ident;
        semanticFailIfTrue(superBinding == SuperBinding::Needed && *stringPropertyName == m_vm->propertyNames->prototype,
            "Cannot declare a static method named 'prototype'");
        semanticFailIfTrue(superBinding == SuperBinding::Needed && *stringPropertyName == m_vm->propertyNames->constructor,
            "Cannot declare a getter or setter named 'constructor'");
        next();
    } else if (match(DOUBLE) || match(INTEGER)) {
        numericPropertyName = m_token.m_data.doubleValue;
        next();
    } else if (match(OPENBRACKET)) {
        next();
        computedPropertyName = parseAssignmentExpression(context);
        failIfFalse(computedPropertyName, "Cannot parse computed property name");
        handleProductionOrFail(CLOSEBRACKET, "]", "end", "computed property name");
    } else
        failDueToUnexpectedToken();

    ParserFunctionInfo<TreeBuilder> info;
    if (type & PropertyNode::Getter) {
        failIfFalse(match(OPENPAREN), "Expected a parameter list for getter definition");
        failIfFalse((parseFunctionInfo(context, FunctionNoRequirements, SourceParseMode::GetterMode, false, constructorKind, superBinding, getterOrSetterStartOffset, info, FunctionDefinitionType::Method)), "Cannot parse getter definition");
    } else {
        failIfFalse(match(OPENPAREN), "Expected a parameter list for setter definition");
        failIfFalse((parseFunctionInfo(context, FunctionNoRequirements, SourceParseMode::SetterMode, false, constructorKind, superBinding, getterOrSetterStartOffset, info, FunctionDefinitionType::Method)), "Cannot parse setter definition");
    }

    if (stringPropertyName)
        return context.createGetterOrSetterProperty(location, type, strict, stringPropertyName, info, superBinding);

    if (computedPropertyName)
        return context.createGetterOrSetterProperty(location, static_cast<PropertyNode::Type>(type | PropertyNode::Computed), strict, computedPropertyName, info, superBinding);

    return context.createGetterOrSetterProperty(const_cast<VM*>(m_vm), m_parserArena, location, type, strict, numericPropertyName, info, superBinding);
}

template <typename LexerType>
template <class TreeBuilder> bool Parser<LexerType>::shouldCheckPropertyForUnderscoreProtoDuplicate(TreeBuilder& context, const TreeProperty& property)
{
    if (m_syntaxAlreadyValidated)
        return false;

    if (!context.getName(property))
        return false;

    // A Constant property that is not a Computed or Shorthand Constant property.
    return context.getType(property) == PropertyNode::Constant;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseObjectLiteral(TreeBuilder& context)
{
    SavePoint savePoint = createSavePoint();
    consumeOrFailWithFlags(OPENBRACE, TreeBuilder::DontBuildStrings, "Expected opening '{' at the start of an object literal");

    int oldNonLHSCount = m_parserState.nonLHSCount;

    JSTokenLocation location(tokenLocation());    
    if (match(CLOSEBRACE)) {
        next();
        return context.createObjectLiteral(location);
    }
    
    TreeProperty property = parseProperty(context, false);
    failIfFalse(property, "Cannot parse object literal property");

    if (!m_syntaxAlreadyValidated && context.getType(property) & (PropertyNode::Getter | PropertyNode::Setter)) {
        restoreSavePoint(savePoint);
        return parseStrictObjectLiteral(context);
    }

    bool seenUnderscoreProto = false;
    if (shouldCheckPropertyForUnderscoreProtoDuplicate(context, property))
        seenUnderscoreProto = *context.getName(property) == m_vm->propertyNames->underscoreProto;

    TreePropertyList propertyList = context.createPropertyList(location, property);
    TreePropertyList tail = propertyList;
    while (match(COMMA)) {
        next(TreeBuilder::DontBuildStrings);
        if (match(CLOSEBRACE))
            break;
        JSTokenLocation propertyLocation(tokenLocation());
        property = parseProperty(context, false);
        failIfFalse(property, "Cannot parse object literal property");
        if (!m_syntaxAlreadyValidated && context.getType(property) & (PropertyNode::Getter | PropertyNode::Setter)) {
            restoreSavePoint(savePoint);
            return parseStrictObjectLiteral(context);
        }
        if (shouldCheckPropertyForUnderscoreProtoDuplicate(context, property)) {
            if (*context.getName(property) == m_vm->propertyNames->underscoreProto) {
                semanticFailIfTrue(seenUnderscoreProto, "Attempted to redefine __proto__ property");
                seenUnderscoreProto = true;
            }
        }
        tail = context.createPropertyList(propertyLocation, property, tail);
    }

    location = tokenLocation();
    handleProductionOrFail(CLOSEBRACE, "}", "end", "object literal");
    
    m_parserState.nonLHSCount = oldNonLHSCount;
    
    return context.createObjectLiteral(location, propertyList);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseStrictObjectLiteral(TreeBuilder& context)
{
    consumeOrFail(OPENBRACE, "Expected opening '{' at the start of an object literal");
    
    int oldNonLHSCount = m_parserState.nonLHSCount;

    JSTokenLocation location(tokenLocation());
    if (match(CLOSEBRACE)) {
        next();
        return context.createObjectLiteral(location);
    }
    
    TreeProperty property = parseProperty(context, true);
    failIfFalse(property, "Cannot parse object literal property");

    bool seenUnderscoreProto = false;
    if (shouldCheckPropertyForUnderscoreProtoDuplicate(context, property))
        seenUnderscoreProto = *context.getName(property) == m_vm->propertyNames->underscoreProto;

    TreePropertyList propertyList = context.createPropertyList(location, property);
    TreePropertyList tail = propertyList;
    while (match(COMMA)) {
        next();
        if (match(CLOSEBRACE))
            break;
        JSTokenLocation propertyLocation(tokenLocation());
        property = parseProperty(context, true);
        failIfFalse(property, "Cannot parse object literal property");
        if (shouldCheckPropertyForUnderscoreProtoDuplicate(context, property)) {
            if (*context.getName(property) == m_vm->propertyNames->underscoreProto) {
                semanticFailIfTrue(seenUnderscoreProto, "Attempted to redefine __proto__ property");
                seenUnderscoreProto = true;
            }
        }
        tail = context.createPropertyList(propertyLocation, property, tail);
    }

    location = tokenLocation();
    handleProductionOrFail(CLOSEBRACE, "}", "end", "object literal");

    m_parserState.nonLHSCount = oldNonLHSCount;

    return context.createObjectLiteral(location, propertyList);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseArrayLiteral(TreeBuilder& context)
{
    consumeOrFailWithFlags(OPENBRACKET, TreeBuilder::DontBuildStrings, "Expected an opening '[' at the beginning of an array literal");
    
    int oldNonLHSCount = m_parserState.nonLHSCount;
    
    int elisions = 0;
    while (match(COMMA)) {
        next(TreeBuilder::DontBuildStrings);
        elisions++;
    }
    if (match(CLOSEBRACKET)) {
        JSTokenLocation location(tokenLocation());
        next(TreeBuilder::DontBuildStrings);
        return context.createArray(location, elisions);
    }
    
    TreeExpression elem;
    if (UNLIKELY(match(DOTDOTDOT))) {
        auto spreadLocation = m_token.m_location;
        auto start = m_token.m_startPosition;
        auto divot = m_token.m_endPosition;
        next();
        auto spreadExpr = parseAssignmentExpressionOrPropagateErrorClass(context);
        failIfFalse(spreadExpr, "Cannot parse subject of a spread operation");
        elem = context.createSpreadExpression(spreadLocation, spreadExpr, start, divot, m_lastTokenEndPosition);
    } else
        elem = parseAssignmentExpressionOrPropagateErrorClass(context);
    failIfFalse(elem, "Cannot parse array literal element");
    typename TreeBuilder::ElementList elementList = context.createElementList(elisions, elem);
    typename TreeBuilder::ElementList tail = elementList;
    elisions = 0;
    while (match(COMMA)) {
        next(TreeBuilder::DontBuildStrings);
        elisions = 0;
        
        while (match(COMMA)) {
            next();
            elisions++;
        }
        
        if (match(CLOSEBRACKET)) {
            JSTokenLocation location(tokenLocation());
            next(TreeBuilder::DontBuildStrings);
            return context.createArray(location, elisions, elementList);
        }
        if (UNLIKELY(match(DOTDOTDOT))) {
            auto spreadLocation = m_token.m_location;
            auto start = m_token.m_startPosition;
            auto divot = m_token.m_endPosition;
            next();
            TreeExpression elem = parseAssignmentExpressionOrPropagateErrorClass(context);
            failIfFalse(elem, "Cannot parse subject of a spread operation");
            auto spread = context.createSpreadExpression(spreadLocation, elem, start, divot, m_lastTokenEndPosition);
            tail = context.createElementList(tail, elisions, spread);
            continue;
        }
        TreeExpression elem = parseAssignmentExpressionOrPropagateErrorClass(context);
        failIfFalse(elem, "Cannot parse array literal element");
        tail = context.createElementList(tail, elisions, elem);
    }

    JSTokenLocation location(tokenLocation());
    if (!consume(CLOSEBRACKET)) {
        failIfFalse(match(DOTDOTDOT), "Expected either a closing ']' or a ',' following an array element");
        semanticFail("The '...' operator should come before a target expression");
    }
    
    m_parserState.nonLHSCount = oldNonLHSCount;
    
    return context.createArray(location, elementList);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseFunctionExpression(TreeBuilder& context)
{
    ASSERT(match(FUNCTION));
    JSTokenLocation location(tokenLocation());
    unsigned functionKeywordStart = tokenStart();
    next();
    ParserFunctionInfo<TreeBuilder> functionInfo;
    functionInfo.name = &m_vm->propertyNames->nullIdentifier;
    SourceParseMode parseMode = SourceParseMode::NormalFunctionMode;
#if ENABLE(ES6_GENERATORS)
    if (consume(TIMES))
        parseMode = SourceParseMode::GeneratorWrapperFunctionMode;
#endif
    failIfFalse((parseFunctionInfo(context, FunctionNoRequirements, parseMode, false, ConstructorKind::None, SuperBinding::NotNeeded, functionKeywordStart, functionInfo, FunctionDefinitionType::Expression)), "Cannot parse function expression");
    return context.createFunctionExpr(location, functionInfo);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::TemplateString Parser<LexerType>::parseTemplateString(TreeBuilder& context, bool isTemplateHead, typename LexerType::RawStringsBuildMode rawStringsBuildMode, bool& elementIsTail)
{
    if (!isTemplateHead) {
        matchOrFail(CLOSEBRACE, "Expected a closing '}' following an expression in template literal");
        // Re-scan the token to recognize it as Template Element.
        m_token.m_type = m_lexer->scanTrailingTemplateString(&m_token, rawStringsBuildMode);
    }
    matchOrFail(TEMPLATE, "Expected an template element");
    const Identifier* cooked = m_token.m_data.cooked;
    const Identifier* raw = m_token.m_data.raw;
    elementIsTail = m_token.m_data.isTail;
    JSTokenLocation location(tokenLocation());
    next();
    return context.createTemplateString(location, *cooked, *raw);
}

template <typename LexerType>
template <class TreeBuilder> typename TreeBuilder::TemplateLiteral Parser<LexerType>::parseTemplateLiteral(TreeBuilder& context, typename LexerType::RawStringsBuildMode rawStringsBuildMode)
{
    JSTokenLocation location(tokenLocation());
    bool elementIsTail = false;

    auto headTemplateString = parseTemplateString(context, true, rawStringsBuildMode, elementIsTail);
    failIfFalse(headTemplateString, "Cannot parse head template element");

    typename TreeBuilder::TemplateStringList templateStringList = context.createTemplateStringList(headTemplateString);
    typename TreeBuilder::TemplateStringList templateStringTail = templateStringList;

    if (elementIsTail)
        return context.createTemplateLiteral(location, templateStringList);

    failIfTrue(match(CLOSEBRACE), "Template literal expression cannot be empty");
    TreeExpression expression = parseExpression(context);
    failIfFalse(expression, "Cannot parse expression in template literal");

    typename TreeBuilder::TemplateExpressionList templateExpressionList = context.createTemplateExpressionList(expression);
    typename TreeBuilder::TemplateExpressionList templateExpressionTail = templateExpressionList;

    auto templateString = parseTemplateString(context, false, rawStringsBuildMode, elementIsTail);
    failIfFalse(templateString, "Cannot parse template element");
    templateStringTail = context.createTemplateStringList(templateStringTail, templateString);

    while (!elementIsTail) {
        failIfTrue(match(CLOSEBRACE), "Template literal expression cannot be empty");
        TreeExpression expression = parseExpression(context);
        failIfFalse(expression, "Cannot parse expression in template literal");

        templateExpressionTail = context.createTemplateExpressionList(templateExpressionTail, expression);

        auto templateString = parseTemplateString(context, false, rawStringsBuildMode, elementIsTail);
        failIfFalse(templateString, "Cannot parse template element");
        templateStringTail = context.createTemplateStringList(templateStringTail, templateString);
    }

    return context.createTemplateLiteral(location, templateStringList, templateExpressionList);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parsePrimaryExpression(TreeBuilder& context)
{
    failIfStackOverflow();
    switch (m_token.m_type) {
    case FUNCTION:
        return parseFunctionExpression(context);
#if ENABLE(ES6_CLASS_SYNTAX)
    case CLASSTOKEN: {
        ParserClassInfo<TreeBuilder> info;
        return parseClass(context, FunctionNoRequirements, info);
    }
#endif
    case OPENBRACE:
        if (strictMode())
            return parseStrictObjectLiteral(context);
        return parseObjectLiteral(context);
    case OPENBRACKET:
        return parseArrayLiteral(context);
    case OPENPAREN: {
        next();
        int oldNonLHSCount = m_parserState.nonLHSCount;
        TreeExpression result = parseExpression(context);
        m_parserState.nonLHSCount = oldNonLHSCount;
        handleProductionOrFail(CLOSEPAREN, ")", "end", "compound expression");
        return result;
    }
    case THISTOKEN: {
        JSTokenLocation location(tokenLocation());
        next();
        return context.createThisExpr(location, m_thisTDZMode);
    }
    case IDENT: {
    identifierExpression:
        JSTextPosition start = tokenStartPosition();
        const Identifier* ident = m_token.m_data.ident;
        JSTokenLocation location(tokenLocation());
        next();
        currentScope()->useVariable(ident, m_vm->propertyNames->eval == *ident);
        m_parserState.lastIdentifier = ident;
        return context.createResolve(location, *ident, start, lastTokenEndPosition());
    }
    case STRING: {
        const Identifier* ident = m_token.m_data.ident;
        JSTokenLocation location(tokenLocation());
        next();
        return context.createString(location, ident);
    }
    case DOUBLE: {
        double d = m_token.m_data.doubleValue;
        JSTokenLocation location(tokenLocation());
        next();
        return context.createDoubleExpr(location, d);
    }
    case INTEGER: {
        double d = m_token.m_data.doubleValue;
        JSTokenLocation location(tokenLocation());
        next();
        return context.createIntegerExpr(location, d);
    }
    case NULLTOKEN: {
        JSTokenLocation location(tokenLocation());
        next();
        return context.createNull(location);
    }
    case TRUETOKEN: {
        JSTokenLocation location(tokenLocation());
        next();
        return context.createBoolean(location, true);
    }
    case FALSETOKEN: {
        JSTokenLocation location(tokenLocation());
        next();
        return context.createBoolean(location, false);
    }
    case DIVEQUAL:
    case DIVIDE: {
        /* regexp */
        const Identifier* pattern;
        const Identifier* flags;
        if (match(DIVEQUAL))
            failIfFalse(m_lexer->scanRegExp(pattern, flags, '='), "Invalid regular expression");
        else
            failIfFalse(m_lexer->scanRegExp(pattern, flags), "Invalid regular expression");
        
        JSTextPosition start = tokenStartPosition();
        JSTokenLocation location(tokenLocation());
        next();
        TreeExpression re = context.createRegExp(location, *pattern, *flags, start);
        if (!re) {
            const char* yarrErrorMsg = Yarr::checkSyntax(pattern->string());
            regexFail(yarrErrorMsg);
        }
        return re;
    }
#if ENABLE(ES6_TEMPLATE_LITERAL_SYNTAX)
    case TEMPLATE:
        return parseTemplateLiteral(context, LexerType::RawStringsBuildMode::DontBuildRawStrings);
#endif
    case YIELD:
        if (!strictMode() && !currentScope()->isGenerator())
            goto identifierExpression;
        failDueToUnexpectedToken();
    case LET:
        if (!strictMode())
            goto identifierExpression;
        FALLTHROUGH;
    default:
        failDueToUnexpectedToken();
    }
}

template <typename LexerType>
template <class TreeBuilder> TreeArguments Parser<LexerType>::parseArguments(TreeBuilder& context)
{
    consumeOrFailWithFlags(OPENPAREN, TreeBuilder::DontBuildStrings, "Expected opening '(' at start of argument list");
    JSTokenLocation location(tokenLocation());
    if (match(CLOSEPAREN)) {
        next(TreeBuilder::DontBuildStrings);
        return context.createArguments();
    }
    auto argumentsStart = m_token.m_startPosition;
    auto argumentsDivot = m_token.m_endPosition;

    ArgumentType argType = ArgumentType::Normal;
    TreeExpression firstArg = parseArgument(context, argType);
    failIfFalse(firstArg, "Cannot parse function argument");
    semanticFailIfTrue(match(DOTDOTDOT), "The '...' operator should come before the target expression");

    bool hasSpread = false;
    if (argType == ArgumentType::Spread)
        hasSpread = true;
    TreeArgumentsList argList = context.createArgumentsList(location, firstArg);
    TreeArgumentsList tail = argList;

    while (match(COMMA)) {
        JSTokenLocation argumentLocation(tokenLocation());
        next(TreeBuilder::DontBuildStrings);

        TreeExpression arg = parseArgument(context, argType);
        propagateError();
        semanticFailIfTrue(match(DOTDOTDOT), "The '...' operator should come before the target expression");

        if (argType == ArgumentType::Spread)
            hasSpread = true;

        tail = context.createArgumentsList(argumentLocation, tail, arg);
    }

    handleProductionOrFail(CLOSEPAREN, ")", "end", "argument list");
    if (hasSpread) {
        TreeExpression spreadArray = context.createSpreadExpression(location, context.createArray(location, context.createElementList(argList)), argumentsStart, argumentsDivot, m_lastTokenEndPosition);
        return context.createArguments(context.createArgumentsList(location, spreadArray));
    }

    return context.createArguments(argList);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseArgument(TreeBuilder& context, ArgumentType& type)
{
    if (UNLIKELY(match(DOTDOTDOT))) {
        JSTokenLocation spreadLocation(tokenLocation());
        auto start = m_token.m_startPosition;
        auto divot = m_token.m_endPosition;
        next();
        TreeExpression spreadExpr = parseAssignmentExpression(context);
        propagateError();
        auto end = m_lastTokenEndPosition;
        type = ArgumentType::Spread;
        return context.createSpreadExpression(spreadLocation, spreadExpr, start, divot, end);
    }

    type = ArgumentType::Normal;
    return parseAssignmentExpression(context);
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseMemberExpression(TreeBuilder& context)
{
    TreeExpression base = 0;
    JSTextPosition expressionStart = tokenStartPosition();
    int newCount = 0;
    JSTokenLocation startLocation = tokenLocation();
    JSTokenLocation location;
    while (match(NEW)) {
        next();
        newCount++;
    }

#if ENABLE(ES6_CLASS_SYNTAX)
    bool baseIsSuper = match(SUPER);
    semanticFailIfTrue(baseIsSuper && newCount, "Cannot use new with super");
#else
    bool baseIsSuper = false;
#endif

    bool baseIsNewTarget = false;
    if (newCount && match(DOT)) {
        next();
        if (match(IDENT)) {
            const Identifier* ident = m_token.m_data.ident;
            if (m_vm->propertyNames->target == *ident) {
                semanticFailIfFalse(currentScope()->isFunction(), "new.target is only valid inside functions");
                baseIsNewTarget = true;
                base = context.createNewTargetExpr(location);
                newCount--;
                next();
            } else
                failWithMessage("\"new.\" can only followed with target");
        } else
            failDueToUnexpectedToken();
    }

    if (baseIsSuper) {
        semanticFailIfFalse(currentScope()->isFunction(), "super is only valid inside functions");
        base = context.createSuperExpr(location);
        next();
        currentFunctionScope()->setNeedsSuperBinding();
    } else if (!baseIsNewTarget)
        base = parsePrimaryExpression(context);

    failIfFalse(base, "Cannot parse base expression");
    while (true) {
        location = tokenLocation();
        switch (m_token.m_type) {
        case OPENBRACKET: {
            m_parserState.nonTrivialExpressionCount++;
            JSTextPosition expressionEnd = lastTokenEndPosition();
            next();
            int nonLHSCount = m_parserState.nonLHSCount;
            int initialAssignments = m_parserState.assignmentCount;
            TreeExpression property = parseExpression(context);
            failIfFalse(property, "Cannot parse subscript expression");
            base = context.createBracketAccess(location, base, property, initialAssignments != m_parserState.assignmentCount, expressionStart, expressionEnd, tokenEndPosition());
            handleProductionOrFail(CLOSEBRACKET, "]", "end", "subscript expression");
            m_parserState.nonLHSCount = nonLHSCount;
            break;
        }
        case OPENPAREN: {
            m_parserState.nonTrivialExpressionCount++;
            int nonLHSCount = m_parserState.nonLHSCount;
            if (newCount) {
                newCount--;
                JSTextPosition expressionEnd = lastTokenEndPosition();
                TreeArguments arguments = parseArguments(context);
                failIfFalse(arguments, "Cannot parse call arguments");
                base = context.createNewExpr(location, base, arguments, expressionStart, expressionEnd, lastTokenEndPosition());
            } else {
                JSTextPosition expressionEnd = lastTokenEndPosition();
                TreeArguments arguments = parseArguments(context);
                failIfFalse(arguments, "Cannot parse call arguments");
                if (baseIsSuper)
                    currentFunctionScope()->setHasDirectSuper();
                base = context.makeFunctionCallNode(startLocation, base, arguments, expressionStart, expressionEnd, lastTokenEndPosition());
            }
            m_parserState.nonLHSCount = nonLHSCount;
            break;
        }
        case DOT: {
            m_parserState.nonTrivialExpressionCount++;
            JSTextPosition expressionEnd = lastTokenEndPosition();
            nextExpectIdentifier(LexerFlagsIgnoreReservedWords | TreeBuilder::DontBuildKeywords);
            matchOrFail(IDENT, "Expected a property name after '.'");
            base = context.createDotAccess(location, base, m_token.m_data.ident, expressionStart, expressionEnd, tokenEndPosition());
            next();
            break;
        }
#if ENABLE(ES6_TEMPLATE_LITERAL_SYNTAX)
        case TEMPLATE: {
            semanticFailIfTrue(baseIsSuper, "Cannot use super as tag for tagged templates");
            JSTextPosition expressionEnd = lastTokenEndPosition();
            int nonLHSCount = m_parserState.nonLHSCount;
            typename TreeBuilder::TemplateLiteral templateLiteral = parseTemplateLiteral(context, LexerType::RawStringsBuildMode::BuildRawStrings);
            failIfFalse(templateLiteral, "Cannot parse template literal");
            base = context.createTaggedTemplate(location, base, templateLiteral, expressionStart, expressionEnd, lastTokenEndPosition());
            m_parserState.nonLHSCount = nonLHSCount;
            break;
        }
#endif
        default:
            goto endMemberExpression;
        }
        baseIsSuper = false;
    }
endMemberExpression:
    semanticFailIfTrue(baseIsSuper, "Cannot reference super");
    while (newCount--)
        base = context.createNewExpr(location, base, expressionStart, lastTokenEndPosition());
    return base;
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseArrowFunctionExpression(TreeBuilder& context)
{
    JSTokenLocation location;

    unsigned functionKeywordStart = tokenStart();
    location = tokenLocation();
    ParserFunctionInfo<TreeBuilder> info;
    info.name = &m_vm->propertyNames->nullIdentifier;
    failIfFalse((parseFunctionInfo(context, FunctionNoRequirements, SourceParseMode::ArrowFunctionMode, true, ConstructorKind::None, SuperBinding::NotNeeded, functionKeywordStart, info, FunctionDefinitionType::Expression)), "Cannot parse arrow function expression");

    return context.createArrowFunctionExpr(location, info);
}

static const char* operatorString(bool prefix, unsigned tok)
{
    switch (tok) {
    case MINUSMINUS:
    case AUTOMINUSMINUS:
        return prefix ? "prefix-decrement" : "decrement";

    case PLUSPLUS:
    case AUTOPLUSPLUS:
        return prefix ? "prefix-increment" : "increment";

    case EXCLAMATION:
        return "logical-not";

    case TILDE:
        return "bitwise-not";
    
    case TYPEOF:
        return "typeof";
    
    case VOIDTOKEN:
        return "void";
    
    case DELETETOKEN:
        return "delete";
    }
    RELEASE_ASSERT_NOT_REACHED();
    return "error";
}

template <typename LexerType>
template <class TreeBuilder> TreeExpression Parser<LexerType>::parseUnaryExpression(TreeBuilder& context)
{
    typename TreeBuilder::UnaryExprContext unaryExprContext(context);
    AllowInOverride allowInOverride(this);
    int tokenStackDepth = 0;
    bool modifiesExpr = false;
    bool requiresLExpr = false;
    unsigned lastOperator = 0;
    while (isUnaryOp(m_token.m_type)) {
        if (strictMode()) {
            switch (m_token.m_type) {
            case PLUSPLUS:
            case MINUSMINUS:
            case AUTOPLUSPLUS:
            case AUTOMINUSMINUS:
                semanticFailIfTrue(requiresLExpr, "The ", operatorString(true, lastOperator), " operator requires a reference expression");
                modifiesExpr = true;
                requiresLExpr = true;
                break;
            case DELETETOKEN:
                semanticFailIfTrue(requiresLExpr, "The ", operatorString(true, lastOperator), " operator requires a reference expression");
                requiresLExpr = true;
                break;
            default:
                semanticFailIfTrue(requiresLExpr, "The ", operatorString(true, lastOperator), " operator requires a reference expression");
                break;
            }
        }
        lastOperator = m_token.m_type;
        m_parserState.nonLHSCount++;
        context.appendUnaryToken(tokenStackDepth, m_token.m_type, tokenStartPosition());
        next();
        m_parserState.nonTrivialExpressionCount++;
    }
    JSTextPosition subExprStart = tokenStartPosition();
    ASSERT(subExprStart.offset >= subExprStart.lineStartOffset);
    JSTokenLocation location(tokenLocation());
    TreeExpression expr = parseMemberExpression(context);
    if (!expr) {
        if (lastOperator)
            failWithMessage("Cannot parse subexpression of ", operatorString(true, lastOperator), "operator");
        failWithMessage("Cannot parse member expression");
    }
    bool isEvalOrArguments = false;
    if (strictMode() && !m_syntaxAlreadyValidated) {
        if (context.isResolve(expr))
            isEvalOrArguments = *m_parserState.lastIdentifier == m_vm->propertyNames->eval || *m_parserState.lastIdentifier == m_vm->propertyNames->arguments;
    }
    failIfTrueIfStrict(isEvalOrArguments && modifiesExpr, "Cannot modify '", m_parserState.lastIdentifier->impl(), "' in strict mode");
    switch (m_token.m_type) {
    case PLUSPLUS:
        m_parserState.nonTrivialExpressionCount++;
        m_parserState.nonLHSCount++;
        expr = context.makePostfixNode(location, expr, OpPlusPlus, subExprStart, lastTokenEndPosition(), tokenEndPosition());
        m_parserState.assignmentCount++;
        failIfTrueIfStrict(isEvalOrArguments, "Cannot modify '", m_parserState.lastIdentifier->impl(), "' in strict mode");
        semanticFailIfTrue(requiresLExpr, "The ", operatorString(false, lastOperator), " operator requires a reference expression");
        lastOperator = PLUSPLUS;
        next();
        break;
    case MINUSMINUS:
        m_parserState.nonTrivialExpressionCount++;
        m_parserState.nonLHSCount++;
        expr = context.makePostfixNode(location, expr, OpMinusMinus, subExprStart, lastTokenEndPosition(), tokenEndPosition());
        m_parserState.assignmentCount++;
        failIfTrueIfStrict(isEvalOrArguments, "'", m_parserState.lastIdentifier->impl(), "' cannot be modified in strict mode");
        semanticFailIfTrue(requiresLExpr, "The ", operatorString(false, lastOperator), " operator requires a reference expression");
        lastOperator = PLUSPLUS;
        next();
        break;
    default:
        break;
    }
    
    JSTextPosition end = lastTokenEndPosition();

    if (!TreeBuilder::CreatesAST && (m_syntaxAlreadyValidated || !strictMode()))
        return expr;

    location = tokenLocation();
    location.line = m_lexer->lastLineNumber();
    while (tokenStackDepth) {
        switch (context.unaryTokenStackLastType(tokenStackDepth)) {
        case EXCLAMATION:
            expr = context.createLogicalNot(location, expr);
            break;
        case TILDE:
            expr = context.makeBitwiseNotNode(location, expr);
            break;
        case MINUS:
            expr = context.makeNegateNode(location, expr);
            break;
        case PLUS:
            expr = context.createUnaryPlus(location, expr);
            break;
        case PLUSPLUS:
        case AUTOPLUSPLUS:
            expr = context.makePrefixNode(location, expr, OpPlusPlus, context.unaryTokenStackLastStart(tokenStackDepth), subExprStart + 1, end);
            m_parserState.assignmentCount++;
            break;
        case MINUSMINUS:
        case AUTOMINUSMINUS:
            expr = context.makePrefixNode(location, expr, OpMinusMinus, context.unaryTokenStackLastStart(tokenStackDepth), subExprStart + 1, end);
            m_parserState.assignmentCount++;
            break;
        case TYPEOF:
            expr = context.makeTypeOfNode(location, expr);
            break;
        case VOIDTOKEN:
            expr = context.createVoid(location, expr);
            break;
        case DELETETOKEN:
            failIfTrueIfStrict(context.isResolve(expr), "Cannot delete unqualified property '", m_parserState.lastIdentifier->impl(), "' in strict mode");
            expr = context.makeDeleteNode(location, expr, context.unaryTokenStackLastStart(tokenStackDepth), end, end);
            break;
        default:
            // If we get here something has gone horribly horribly wrong
            CRASH();
        }
        subExprStart = context.unaryTokenStackLastStart(tokenStackDepth);
        context.unaryTokenStackRemoveLast(tokenStackDepth);
    }
    return expr;
}


template <typename LexerType> void Parser<LexerType>::printUnexpectedTokenText(WTF::PrintStream& out)
{
    switch (m_token.m_type) {
    case EOFTOK:
        out.print("Unexpected end of script");
        return;
    case UNTERMINATED_IDENTIFIER_ESCAPE_ERRORTOK:
    case UNTERMINATED_IDENTIFIER_UNICODE_ESCAPE_ERRORTOK:
        out.print("Incomplete unicode escape in identifier: '", getToken(), "'");
        return;
    case UNTERMINATED_MULTILINE_COMMENT_ERRORTOK:
        out.print("Unterminated multiline comment");
        return;
    case UNTERMINATED_NUMERIC_LITERAL_ERRORTOK:
        out.print("Unterminated numeric literal '", getToken(), "'");
        return;
    case UNTERMINATED_STRING_LITERAL_ERRORTOK:
        out.print("Unterminated string literal '", getToken(), "'");
        return;
    case INVALID_IDENTIFIER_ESCAPE_ERRORTOK:
        out.print("Invalid escape in identifier: '", getToken(), "'");
        return;
    case INVALID_IDENTIFIER_UNICODE_ESCAPE_ERRORTOK:
        out.print("Invalid unicode escape in identifier: '", getToken(), "'");
        return;
    case INVALID_NUMERIC_LITERAL_ERRORTOK:
        out.print("Invalid numeric literal: '", getToken(), "'");
        return;
    case UNTERMINATED_OCTAL_NUMBER_ERRORTOK:
        out.print("Invalid use of octal: '", getToken(), "'");
        return;
    case INVALID_STRING_LITERAL_ERRORTOK:
        out.print("Invalid string literal: '", getToken(), "'");
        return;
    case ERRORTOK:
        out.print("Unrecognized token '", getToken(), "'");
        return;
    case STRING:
        out.print("Unexpected string literal ", getToken());
        return;
    case INTEGER:
    case DOUBLE:
        out.print("Unexpected number '", getToken(), "'");
        return;
    
    case RESERVED_IF_STRICT:
        out.print("Unexpected use of reserved word '", getToken(), "' in strict mode");
        return;
        
    case RESERVED:
        out.print("Unexpected use of reserved word '", getToken(), "'");
        return;

    case INVALID_PRIVATE_NAME_ERRORTOK:
        out.print("Invalid private name '", getToken(), "'");
        return;
            
    case IDENT:
        out.print("Unexpected identifier '", getToken(), "'");
        return;

    default:
        break;
    }

    if (m_token.m_type & KeywordTokenFlag) {
        out.print("Unexpected keyword '", getToken(), "'");
        return;
    }
    
    out.print("Unexpected token '", getToken(), "'");
}

// Instantiate the two flavors of Parser we need instead of putting most of this file in Parser.h
template class Parser<Lexer<LChar>>;
template class Parser<Lexer<UChar>>;
    
} // namespace JSC
