//
//     ,ad888ba,                              88
//    d8"'    "8b
//   d8            88,dba,,adba,   ,aPP8A.A8  88     The Cmajor Toolkit
//   Y8,           88    88    88  88     88  88
//    Y8a.   .a8P  88    88    88  88,   ,88  88     (C)2024 Cmajor Software Ltd
//     '"Y888Y"'   88    88    88  '"8bbP"Y8  88     https://cmajor.dev
//                                           ,88
//                                        888P"
//
//  The Cmajor project is subject to commercial or open-source licensing.
//  You may use it under the terms of the GPLv3 (see www.gnu.org/licenses), or
//  visit https://cmajor.dev to learn about our commercial licence options.
//
//  CMAJOR IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
//  EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
//  DISCLAIMED.

#include "../../include/cmaj_ErrorHandling.h"
#include "../../../../include/cmajor/COM/cmaj_Library.h"
#include "cmaj_AST.h"
#include "cmaj_Lexer.h"
#include "../transformations/cmaj_Transformations.h"
#include "cmaj_Parser.h"
#include "../standard_library/cmaj_StandardLibrary.h"
#include "../standard_library/cmaj_StandardLibraryBinary.h"
#include <filesystem>
#include <fstream>
#include <regex>
#include <algorithm>

namespace cmaj
{
    const char* Library::getVersion()
    {
        // This needs to be set to something sensible
        static_assert (CMAJ_VERSION[0] != 0);
        return CMAJ_VERSION;
    }

    ProgramPtr Library::createProgram()
    {
        return choc::com::create<cmaj::AST::Program>();
    }

    void AST::Program::parse (const SourceFile& source, bool isSystemModule)
    {
        Parser::parseModuleDeclarations (allocator, source, isSystemModule, parsingComments, rootNamespace, {});
        resolveImports (source);
        resetMainProcessor();
    }

    void AST::Program::resolveImports (const SourceFile& importingSource)
    {
        std::vector<std::pair<std::string, std::string>> pendingImports;

        auto collectImportsFrom = [&] (AST::Namespace& ns)
        {
            for (auto& imp : ns.imports)
            {
                auto importPathView = imp->toStdString();
                std::string importPath (importPathView);

                if (importPath.empty())
                    continue;

                bool isFilePath = (importPath.front() == '.' || importPath.find ('/') != std::string::npos
                                   || importPath.find (".cmajor") != std::string::npos);

                if (isFilePath)
                {
                    auto resolvedPath = resolveImportFilePath (importingSource.filename, importPath);

                    if (! resolvedPath.empty() && ! isAlreadyLoaded (resolvedPath))
                    {
                        auto namespaceName = deriveNamespaceName (importPath);
                        pendingImports.push_back ({ resolvedPath, namespaceName });
                    }
                }
                else
                {
                    std::string dotToSlash = importPath;

                    for (auto& c : dotToSlash)
                        if (c == '.')
                            c = '/';

                    if (importFileResolver)
                    {
                        auto result = importFileResolver (dotToSlash);

                        if (! result.empty())
                        {
                            auto lastDot = importPath.rfind ('.');
                            std::string namespaceName = (lastDot != std::string::npos)
                                ? importPath.substr (lastDot + 1) : importPath;

                            for (auto& [path, content] : result)
                                if (! isAlreadyLoaded (path))
                                    loadImportedFile (path, content, namespaceName);
                        }
                    }
                }
            }
        };

        collectImportsFrom (rootNamespace);

        rootNamespace.visitAllModules (false, [&] (AST::ModuleBase& m)
        {
            if (auto ns = m.getAsNamespace())
                collectImportsFrom (*ns);
        });

        for (auto& [path, namespaceName] : pendingImports)
            loadImportedFileFromDisk (path, namespaceName);
    }

    std::string AST::Program::resolveImportFilePath (const std::string& importerPath, const std::string& importPath)
    {
        namespace fs = std::filesystem;

        try
        {
            fs::path importer (importerPath);
            fs::path base = importer.parent_path();
            fs::path resolved = base / importPath;

            if (fs::exists (resolved))
                return fs::canonical (resolved).string();
        }
        catch (...) {}

        return {};
    }

    std::string AST::Program::deriveNamespaceName (const std::string& importPath)
    {
        namespace fs = std::filesystem;
        auto stem = fs::path (importPath).stem().string();

        std::string result;
        for (auto c : stem)
            if (std::isalnum (static_cast<unsigned char> (c)) || c == '_')
                result += c;

        if (result.empty() || std::isdigit (static_cast<unsigned char> (result[0])))
            result = "_" + result;

        return result;
    }

    bool AST::Program::isAlreadyLoaded (const std::string& filePath)
    {
        for (auto& sf : allocator.sourceFileList.sourceFiles)
            if (sf->filename == filePath)
                return true;

        return false;
    }

    void AST::Program::loadImportedFileFromDisk (const std::string& filePath, const std::string& namespaceName)
    {
        try
        {
            std::ifstream file (filePath);

            if (! file.is_open())
                return;

            std::string content ((std::istreambuf_iterator<char> (file)),
                                  std::istreambuf_iterator<char>());

            loadImportedFile (filePath, content, namespaceName);
        }
        catch (...) {}
    }

    void AST::Program::loadImportedFile (const std::string& filePath, const std::string& content,
                                          const std::string& namespaceName)
    {
        auto wrappedContent = "namespace " + namespaceName + " {\n" + content + "\n}\n";

        auto mainPattern = std::regex (R"(\[\[\s*main\s*\]\])");
        wrappedContent = std::regex_replace (wrappedContent, mainPattern, "");

        auto& sourceFile = allocator.sourceFileList.add (filePath, std::move (wrappedContent), false);
        Parser::parseModuleDeclarations (allocator, sourceFile, false, parsingComments, rootNamespace, {});
        codeHash.addInput (sourceFile.content);
    }

    void AST::Program::addStandardLibraryCode()
    {
        for (auto& m : transformations::parseBinaryModule (allocator, standardLibraryData, sizeof (standardLibraryData), false))
            rootNamespace.subModules.addChildObject (m);

        transformations::mergeDuplicateNamespaces (rootNamespace);
    }
}
