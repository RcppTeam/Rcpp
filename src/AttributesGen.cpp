// -*- mode: C++; c-indent-level: 4; c-basic-offset: 4; indent-tabs-mode: nil; -*-
//
// AttributesGen.cpp: Rcpp R/C++ interface class library -- Rcpp attributes
//
// Copyright (C) 2012 JJ Allaire, Dirk Eddelbuettel and Romain Francois
//
// This file is part of Rcpp.
//
// Rcpp is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// Rcpp is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Rcpp.  If not, see <http://www.gnu.org/licenses/>.

#include "AttributesGen.h"
#include "AttributesUtil.h"
#include "AttributesTypes.h"

#include <fstream>

#include <Rcpp/iostream/Rostream.h>
#include <Rcpp/exceptions.h>

namespace Rcpp {
namespace attributes {
    
    // constants
    namespace {
        const char * const kRcppExportsSuffix = "_RcppExports.h";
    } 
    
    ExportsGenerator::ExportsGenerator(const std::string& targetFile, 
                                       const std::string& package,
                                       const std::string& commentPrefix)
        : targetFile_(targetFile), 
          package_(package),
          commentPrefix_(commentPrefix),
          hasCppInterface_(false) {
        
        // read the existing target file if it exists
        if (FileInfo(targetFile_).exists()) {
            std::ifstream ifs(targetFile_.c_str());
            if (ifs.fail())
                throw Rcpp::file_io_error(targetFile_);
            std::stringstream buffer;
            buffer << ifs.rdbuf();
            existingCode_ = buffer.str();
        }
        
        // see if this is safe to overwite and throw if it isn't
        if (!isSafeToOverwrite())
            throw Rcpp::file_exists(targetFile_);
    }
    
    void ExportsGenerator::writeFunctions(
                                const SourceFileAttributes& attributes,
                                bool verbose) {
        
        if (attributes.hasInterface(kInterfaceCpp))
            hasCppInterface_ = true;
            
        doWriteFunctions(attributes, verbose);                                
    }
    
    // Commit the stream -- is a no-op if the existing code is identical
    // to the generated code. Returns true if data was written and false
    // if it wasn't (throws exception on io error)
    bool ExportsGenerator::commit(const std::string& preamble) {
        
        // get the generated code
        std::string code = codeStream_.str();
        
        // if there is no generated code AND the exports file does not 
        // currently exist then do nothing
        if (code.empty() && !FileInfo(targetFile_).exists())
            return false;
        
        // write header/preamble
        std::ostringstream headerStream;
        headerStream << commentPrefix_ << " This file was generated by "
                     << "Rcpp::compileAttributes" << std::endl;
        headerStream << commentPrefix_ << " Generator token: " 
                     << generatorToken() << std::endl << std::endl;      
        if (!preamble.empty())
            headerStream << preamble;
            
        // get generated code and only write it if there was a change
        std::string generatedCode = headerStream.str() + code;        
        if (generatedCode != existingCode_) {
            // open the file
            std::ofstream ofs(targetFile_.c_str(), 
                              std::ofstream::out | std::ofstream::trunc);
            if (ofs.fail())
                throw Rcpp::file_io_error(targetFile_);
            
            // write generated code and return
            ofs << generatedCode;
            ofs.close();
            return true;
        } 
        else {
            return false;
        }
    }
    
    // Remove the generated file entirely
    bool ExportsGenerator::remove() {
        return removeFile(targetFile_);
    }    
    
    CppExportsGenerator::CppExportsGenerator(const std::string& packageDir, 
                                             const std::string& package,
                                             const std::string& fileSep)
        : ExportsGenerator( 
            packageDir + fileSep + "src" +  fileSep + "RcppExports.cpp", 
            package,
            "//") 
    {
    }
       
    void CppExportsGenerator::doWriteFunctions(
                                 const SourceFileAttributes& attributes,
                                 bool verbose) {
        
        // generate functions
        generateCpp(ostr(), attributes, true, package());
         
        // track cppExports and signatures (we use these at the end to
        // generate the ValidateSignature and RegisterCCallable functions)
        if (attributes.hasInterface(kInterfaceCpp)) {
            for (SourceFileAttributes::const_iterator 
                       it = attributes.begin(); it != attributes.end(); ++it) {
                if (it->isExportedFunction()) {
                    // add it to the list if it's not hidden
                    Function fun = it->function().renamedTo(it->exportedName());
                    if (!fun.isHidden())
                        cppExports_.push_back(*it);
                }
            }
        }
     
        // verbose if requested
        if (verbose) {
            Rcpp::Rcout << "Exports from " << attributes.sourceFile() << ":" 
                        << std::endl;
            for (std::vector<Attribute>::const_iterator 
                    it = attributes.begin(); it != attributes.end(); ++it) {
                if (it->isExportedFunction())
                    Rcpp::Rcout << "   " << it->function() << std::endl; 
            }
            Rcpp::Rcout << std::endl;   
        }
    }
    
    void CppExportsGenerator::writeEnd()
    {
        // generate a function that can be used to validate exported
        // functions and their signatures prior to looking up with
        // GetCppCallable (otherwise inconsistent signatures between
        // client and library would cause a crash)
        if (hasCppInterface()) {
            
            ostr() << std::endl;
            ostr() << "// validate"
                   << " (ensure exported C++ functions exist before "
                   << "calling them)" << std::endl;
            ostr() << "static int " << exportValidationFunctionRegisteredName()
                   << "(const char* sig) { " << std::endl;
            ostr() << "    static std::set<std::string> signatures;" 
                   << std::endl;
            ostr() << "    if (signatures.empty()) {" << std::endl;
            
            for (std::size_t i=0;i<cppExports_.size(); i++) {
                const Attribute& attr = cppExports_[i];
                ostr() << "        signatures.insert(\"" 
                       << attr.function().signature(attr.exportedName())
                       << "\");" << std::endl;
            }
            ostr() << "    }" << std::endl;
            ostr() << "    return signatures.find(sig) != signatures.end();"
                   << std::endl;
            ostr() << "}" << std::endl;   
            
            // generate a function that will register all of our C++ 
            // exports as C-callable from other packages
            ostr() << std::endl;
            ostr() << "// registerCCallable (register entry points for "
                      "exported C++ functions)" << std::endl;
            ostr() << "RcppExport SEXP " << registerCCallableExportedName()
                   << "() { " << std::endl;
            for (std::size_t i=0;i<cppExports_.size(); i++) {
                const Attribute& attr = cppExports_[i];
                std::string name = package() + "_" + attr.exportedName();
                ostr() << registerCCallable(4, 
                                            attr.exportedName(),
                                            attr.function().name());
                ostr() << std::endl;
            }
            ostr() << registerCCallable(4, 
                                        exportValidationFunction(),
                                        exportValidationFunction());
            ostr() << std::endl;
            ostr() << "    return R_NilValue;" << std::endl;
            ostr() << "}" << std::endl;
         }
    }
    
    std::string CppExportsGenerator::registerCCallable(
                                        size_t indent,
                                        const std::string& exportedName,
                                        const std::string& name) const {
        std::ostringstream ostr;
        std::string indentStr(indent, ' ');
        ostr <<  indentStr << "R_RegisterCCallable(\"" << package() << "\", "
              << "\"" << package() << "_" << exportedName << "\", "
              << "(DL_FUNC)" << package() << "_" << name << ");";  
        return ostr.str();                  
    }
    
    bool CppExportsGenerator::commit(const std::vector<std::string>& includes) {
                   
        // includes 
        std::ostringstream ostr;
        if (!includes.empty()) {
            for (std::size_t i=0;i<includes.size(); i++)
                ostr << includes[i] << std::endl;
        }
        ostr << "#include <string>" << std::endl;
        ostr << "#include <set>" << std::endl;
        ostr << std::endl;
        
        // always bring in Rcpp
        ostr << "using namespace Rcpp;" << std::endl << std::endl;
        
        // commit with preamble
        return ExportsGenerator::commit(ostr.str());                  
    }
    
    CppExportsIncludeGenerator::CppExportsIncludeGenerator(
                                            const std::string& packageDir, 
                                            const std::string& package,
                                            const std::string& fileSep)
        : ExportsGenerator( 
            packageDir +  fileSep + "inst" +  fileSep + "include" +
            fileSep + package + kRcppExportsSuffix, 
            package,
            "//")
    {
        includeDir_ = packageDir +  fileSep + "inst" +  fileSep + "include";
    }
        
    void CppExportsIncludeGenerator::writeBegin() {
        
        ostr() << "namespace " << package() << " {" 
               << std::endl << std::endl;
        
        // Import Rcpp into this namespace. This allows declarations to
        // be written without fully qualifying all Rcpp types. The only
        // negative side-effect is that when this package's namespace
        // is imported it will also pull in Rcpp. However since this is
        // opt-in and represents a general desire to do namespace aliasing
        // this seems okay
        ostr() << "    using namespace Rcpp;" << std::endl << std::endl;
      
        // Write our export validation helper function. Putting it in 
        // an anonymous namespace will hide it from callers and give
        // it per-translation unit linkage
        ostr() << "    namespace {" << std::endl;
        ostr() << "        void validateSignature(const char* sig) {" 
               << std::endl;
        ostr() << "            Rcpp::Function require = "
               << "Rcpp::Environment::base_env()[\"require\"];"
               << std::endl;
        ostr() << "            require(\"" << package() << "\", "
               << "Rcpp::Named(\"quietly\") = true);"
               << std::endl;
        
        std::string validate = "validate";
        std::string fnType = "Ptr_" + validate;
        ostr() << "            typedef int(*" << fnType << ")(const char*);"
               << std::endl;
               
        std::string ptrName = "p_" + validate;
        ostr() << "            static " << fnType << " " << ptrName << " = "
               << "(" << fnType << ")" << std::endl
               << "                " 
               << getCCallable(exportValidationFunctionRegisteredName())
               << ";" << std::endl;
        ostr() << "            if (!" << ptrName << "(sig)) {" << std::endl;
        ostr() << "                throw Rcpp::function_not_exported("
               << std::endl
               << "                    "
               << "\"C++ function with signature '\" + std::string(sig) + \"' not found in " << package()
               << "\");" << std::endl;
        ostr() << "            }" << std::endl;
        ostr() << "        }" << std::endl;
        
        ostr() << "    }" << std::endl << std::endl;
    }
        
    void CppExportsIncludeGenerator::doWriteFunctions(
                                    const SourceFileAttributes& attributes,
                                    bool verbose) {
                                    
        // don't write anything if there is no C++ interface
        if (!attributes.hasInterface(kInterfaceCpp)) 
            return;
                                    
        for(std::vector<Attribute>::const_iterator 
            it = attributes.begin(); it != attributes.end(); ++it) {
     
            if (it->isExportedFunction()) {
                
                Function function = 
                    it->function().renamedTo(it->exportedName());
                    
                // if it's hidden then don't generate a C++ interface
                if (function.isHidden())
                    continue;  
                
                ostr() << "    inline " << function << " {" 
                        << std::endl;
                
                std::string fnType = "Ptr_" + function.name();
                ostr() << "        typedef SEXP(*" << fnType << ")(";
                for (size_t i=0; i<function.arguments().size(); i++) {
                    ostr() << "SEXP";
                    if (i != (function.arguments().size()-1))
                        ostr() << ",";
                }
                ostr() << ");" << std::endl;
                
                std::string ptrName = "p_" + function.name();
                ostr() << "        static " << fnType << " " 
                       << ptrName << " = NULL;" 
                       << std::endl;   
                ostr() << "        if (" << ptrName << " == NULL) {" 
                       << std::endl;
                ostr() << "            validateSignature"
                       << "(\"" << function.signature() << "\");" 
                       << std::endl;
                ostr() << "            " << ptrName << " = "
                       << "(" << fnType << ")"
                       << getCCallable(package() + "_" + function.name()) << ";" 
                       << std::endl;
                ostr() << "        }" << std::endl;
                
                ostr() << "        SEXP resultSEXP = " << ptrName << "(";
                
                
                const std::vector<Argument>& args = function.arguments();
                for (std::size_t i = 0; i<args.size(); i++) {
                    ostr() << "Rcpp::wrap(" << args[i].name() << ")";
                    if (i != (args.size()-1))
                        ostr() << ", ";
                }
                       
                ostr() << ");" << std::endl;
                
                ostr() << "        return Rcpp::as<" << function.type() << " >"
                       << "(resultSEXP);" << std::endl;
                
                ostr() << "    }" << std::endl << std::endl;
            } 
        }                           
    }
    
    void CppExportsIncludeGenerator::writeEnd() {
        ostr() << "}" << std::endl;
        ostr() << std::endl;
        ostr() << "#endif // " << getHeaderGuard() << std::endl;
    }
    
    bool CppExportsIncludeGenerator::commit(
                                    const std::vector<std::string>& includes) {
        
        if (hasCppInterface()) {
            
            // create the include dir if necessary
            createDirectory(includeDir_);
            
            // generate preamble 
            std::ostringstream ostr;
            
            // header guard
            std::string guard = getHeaderGuard();
            ostr << "#ifndef " << guard << std::endl;
            ostr << "#define " << guard << std::endl << std::endl; 
            
            // includes
            if (!includes.empty()) {
                for (std::size_t i=0;i<includes.size(); i++)
                    ostr << includes[i] << std::endl;
                ostr << std::endl;
            }
            
            // commit with preamble
            return ExportsGenerator::commit(ostr.str());
        }
        else {
            return ExportsGenerator::remove();
        }
    }
        
    std::string CppExportsIncludeGenerator::getCCallable(
                                        const std::string& function) const {
        std::ostringstream ostr;
        ostr << "R_GetCCallable"
             << "(\"" << package() << "\", "
             << "\"" << function << "\")";
        return ostr.str();
    }
    
    std::string CppExportsIncludeGenerator::getHeaderGuard() const {
        return "__" + package() + "_RcppExports_h__";
    }
    
    CppPackageIncludeGenerator::CppPackageIncludeGenerator(
                                            const std::string& packageDir, 
                                            const std::string& package,
                                            const std::string& fileSep)
        : ExportsGenerator( 
            packageDir +  fileSep + "inst" +  fileSep + "include" +
            fileSep + package + ".h", 
            package,
            "//")
    {
        includeDir_ = packageDir +  fileSep + "inst" +  fileSep + "include";
    }
    
    void CppPackageIncludeGenerator::writeEnd() {
        if (hasCppInterface()) {
            // header guard
            std::string guard = getHeaderGuard();
            ostr() << "#ifndef " << guard << std::endl;
            ostr() << "#define " << guard << std::endl << std::endl; 
            
            ostr() << "#include \"" << package() << kRcppExportsSuffix 
                   << "\"" << std::endl;
            
            ostr() << std::endl;
            ostr() << "#endif // " << getHeaderGuard() << std::endl;
        }
    }
    
    bool CppPackageIncludeGenerator::commit(
                                const std::vector<std::string>& includes) {
        
        if (hasCppInterface()) {
            
            // create the include dir if necessary
            createDirectory(includeDir_);
            
            // commit 
            return ExportsGenerator::commit();
        }
        else {
            return ExportsGenerator::remove();
        }
    }
    
    std::string CppPackageIncludeGenerator::getHeaderGuard() const {
        return "__" + package() + "_h__";
    }
    
    RExportsGenerator::RExportsGenerator(const std::string& packageDir,
                                         const std::string& package,
                                         const std::string& fileSep)
        : ExportsGenerator(
            packageDir + fileSep + "R" +  fileSep + "RcppExports.R", 
            package,
            "#")
    {
    }
    
    void RExportsGenerator::doWriteFunctions(
                                        const SourceFileAttributes& attributes,
                                        bool verbose) {
        
        if (attributes.hasInterface(kInterfaceR)) {    
            // process each attribute
            for(std::vector<Attribute>::const_iterator 
                it = attributes.begin(); it != attributes.end(); ++it) {
                
                // alias the attribute and function (bail if not export)
                const Attribute& attribute = *it;
                if (!attribute.isExportedFunction())
                    continue;
                const Function& function = attribute.function();
                    
                // print roxygen lines
                for (size_t i=0; i<attribute.roxygen().size(); i++)
                    ostr() << attribute.roxygen()[i] << std::endl;
                        
                // build the parameter list 
                std::string args = generateRArgList(function);
                
                // determine the function name
                std::string name = attribute.exportedName();
                    
                // write the function
                ostr() << name << " <- function(" << args << ") {" 
                       << std::endl;
                ostr() << "    ";
                if (function.type().isVoid())
                    ostr() << "invisible(";
                ostr() << ".Call(";
                ostr() << "'" << package() << "_" << function.name() << "', "
                       << "PACKAGE = '" << package() << "'";
                
                // add arguments
                const std::vector<Argument>& arguments = function.arguments();
                for (size_t i = 0; i<arguments.size(); i++)
                    ostr() << ", " << arguments[i].name();
                ostr() << ")";
                if (function.type().isVoid())
                    ostr() << ")";
                ostr() << std::endl;
            
                ostr() << "}" << std::endl << std::endl;
            }           
        }                      
    }
    
    void RExportsGenerator::writeEnd() { 
        if (hasCppInterface()) {
             // register all C-callable functions
            ostr() << "# Register entry points for exported C++ functions"
                   << std::endl;
            ostr() << "methods::setLoadAction(function(ns) {" << std::endl;
            ostr() << "    .Call('" << registerCCallableExportedName()
                   << "', PACKAGE = '" << package() << "')" 
                   << std::endl << "})" << std::endl;
        }
    }
    
    bool RExportsGenerator::commit(const std::vector<std::string>& includes) {
        return ExportsGenerator::commit();                    
    }
    
    ExportsGenerators::~ExportsGenerators() {
        try {
            for(Itr it = generators_.begin(); it != generators_.end(); ++it)
                delete *it;
            generators_.clear(); 
        }
        catch(...) {}
    }
    
    void ExportsGenerators::add(ExportsGenerator* pGenerator) {
        generators_.push_back(pGenerator);
    }
    
    void ExportsGenerators::writeBegin() {
        for(Itr it = generators_.begin(); it != generators_.end(); ++it)
            (*it)->writeBegin();
    }
    
    void ExportsGenerators::writeFunctions(
                                const SourceFileAttributes& attributes,
                                bool verbose) {
        for(Itr it = generators_.begin(); it != generators_.end(); ++it)
            (*it)->writeFunctions(attributes, verbose);
    }
    
    void ExportsGenerators::writeEnd() {
        for(Itr it = generators_.begin(); it != generators_.end(); ++it)
            (*it)->writeEnd();
    }
    
    // Commit and return a list of the files that were updated
    std::vector<std::string> ExportsGenerators::commit(
                            const std::vector<std::string>& includes) {
        
        std::vector<std::string> updated;
        
        for(Itr it = generators_.begin(); it != generators_.end(); ++it) {
            if ((*it)->commit(includes))
                updated.push_back((*it)->targetFile());
        }
           
        return updated;
    }
    
    // Remove and return a list of files that were removed
    std::vector<std::string> ExportsGenerators::remove() {
        std::vector<std::string> removed;
        for(Itr it = generators_.begin(); it != generators_.end(); ++it) {
            if ((*it)->remove())
                removed.push_back((*it)->targetFile());
        }
        return removed;
    }
    
    
    // Helpers for converting C++  default arguments to R default arguments
    namespace {
        
        // convert a C++ numeric argument to an R argument value 
        // (returns empty string if no conversion is possible)
        std::string cppNumericArgToRArg(const std::string& type,
                                        const std::string& cppArg) {
            // check for a number
            double num;
            std::stringstream argStream(cppArg);
            if ((argStream >> num)) {
                
                // L suffix means return the value literally
                if (!argStream.eof()) {
                    std::string suffix;
                    argStream >> suffix;
                    if (argStream.eof() && suffix == "L")
                        return cppArg;
                }
                
                // no decimal and the type isn't explicitly double or 
                // float means integer
                if (cppArg.find('.') == std::string::npos &&
                    type != "double" && type != "float")
                    return cppArg + "L";
                 
                // otherwise return arg literally
                else
                    return cppArg;
            }   
            else {
                return std::string();
            }
        }
        
        // convert a C++ ::create style argument value to an R argument
        // value (returns empty string if no conversion is possible)
        std::string cppCreateArgToRArg(const std::string& cppArg) {
            
            std::string create = "::create";
            size_t createLoc = cppArg.find(create);
            if (createLoc == std::string::npos ||
                ((createLoc + create.length()) >= cppArg.size())) {
                return std::string();
            }
                
            std::string type = cppArg.substr(0, createLoc);
            std::string rcppScope = "Rcpp::";
            size_t rcppLoc = type.find(rcppScope);
            if (rcppLoc == 0 && type.size() > rcppScope.length())
                type = type.substr(rcppScope.length());
            
            std::string args = cppArg.substr(createLoc + create.length());
            if (type == "CharacterVector")
                return "character" + args;
            else if (type == "IntegerVector")
                return "integer" + args;
            else if (type == "NumericVector")
                return "numeric" + args;
            else    
                return std::string();
        }
        
        // convert a C++ Matrix to an R argument (returns emtpy string
        // if no conversion possible)
        std::string cppMatrixArgToRArg(const std::string& cppArg) {
            
            // look for Matrix
            std::string matrix = "Matrix";
            size_t matrixLoc = cppArg.find(matrix);
            if (matrixLoc == std::string::npos ||
                ((matrixLoc + matrix.length()) >= cppArg.size())) {
                return std::string();
            }
            
            std::string args = cppArg.substr(matrixLoc + matrix.length());
            return "matrix" + args;
        }
        
        // convert a C++ literal to an R argument (returns emtpy string
        // if no conversion possible)
        std::string cppLiteralArgToRArg(const std::string& cppArg) {
            if (cppArg == "true")
                return "TRUE";
            else if (cppArg == "false")
                return "FALSE";
            else if (cppArg == "R_NilValue")
                return "NULL";
            else if (cppArg == "NA_STRING" || cppArg == "NA_INTEGER" ||
                     cppArg == "NA_LOGICAL" || cppArg == "NA_REAL") {
                return "NA";
            }
            else
                return std::string();
        }
        
        // convert a C++ argument value to an R argument value (returns empty
        // string if no conversion is possible)
        std::string cppArgToRArg(const std::string& type,
                                 const std::string& cppArg) {
            
            // try for quoted string
            if (isQuoted(cppArg))
                return cppArg;
            
            // try for literal
            std::string rArg = cppLiteralArgToRArg(cppArg);
            if (!rArg.empty())
                return rArg;
            
            // try for a create arg
            rArg = cppCreateArgToRArg(cppArg);
            if (!rArg.empty())
                return rArg;
                
            // try for a matrix arg
            rArg = cppMatrixArgToRArg(cppArg);
            if (!rArg.empty())
                return rArg;
                
            // try for a numeric arg
            rArg = cppNumericArgToRArg(type, cppArg);
            if (!rArg.empty())
                return rArg;
                
            // couldn't parse the arg
            return std::string();
        }
        
    } // anonymous namespace
        
    // Generate an R argument list for a function
    std::string generateRArgList(const Function& function) {
        std::ostringstream argsOstr;
        const std::vector<Argument>& arguments = function.arguments();
        for (size_t i = 0; i<arguments.size(); i++) {
            const Argument& argument = arguments[i];
            argsOstr << argument.name();
            if (!argument.defaultValue().empty()) {
                std::string rArg = cppArgToRArg(argument.type().name(), 
                                                argument.defaultValue());
                if (!rArg.empty()) {
                    argsOstr << " = " << rArg;
                } else {
                    showWarning("Unable to parse C++ default value '" +
                                argument.defaultValue() + "' for argument "+
                                argument.name() + " of function " +
                                function.name());
                }
            }
               
            if (i != (arguments.size()-1))
                argsOstr << ", ";
        }
        return argsOstr.str();
    }
    
    // Generate the C++ code required to make [[Rcpp::export]] functions
    // available as C symbols with SEXP parameters and return
    void generateCpp(std::ostream& ostr,
                     const SourceFileAttributes& attributes,
                     bool includePrototype,
                     const std::string& contextId) {
        
        // process each attribute
        for(std::vector<Attribute>::const_iterator 
            it = attributes.begin(); it != attributes.end(); ++it) {
            
            // alias the attribute and function (bail if not export)
            const Attribute& attribute = *it;
            if (!attribute.isExportedFunction())
                continue;
            const Function& function = attribute.function();
                      
            // include prototype if requested
            if (includePrototype) {
                ostr << "// " << function.name() << std::endl;
                ostr << function << ";";
            }
               
            // write the SEXP-based function
            ostr << std::endl << "RcppExport SEXP ";
            if (!contextId.empty())
                ostr << contextId << "_";
            ostr << function.name() << "(";
            const std::vector<Argument>& arguments = function.arguments();
            for (size_t i = 0; i<arguments.size(); i++) {
                const Argument& argument = arguments[i];
                ostr << "SEXP " << argument.name() << "SEXP";
                if (i != (arguments.size()-1))
                    ostr << ", ";
            }
            ostr << ") {" << std::endl;
            ostr << "BEGIN_RCPP" << std::endl;
            for (size_t i = 0; i<arguments.size(); i++) {
                const Argument& argument = arguments[i];
                
                // Rcpp::as to c++ type
                ostr << "    " << argument.type().name() << " " << argument.name() 
                     << " = " << "Rcpp::as<"  << argument.type().name() << " >(" 
                     << argument.name() << "SEXP);" << std::endl;
            }
            
            ostr << "    ";
            if (!function.type().isVoid())
                ostr << function.type() << " result = ";
            ostr << function.name() << "(";
            for (size_t i = 0; i<arguments.size(); i++) {
                const Argument& argument = arguments[i];
                ostr << argument.name();
                if (i != (arguments.size()-1))
                    ostr << ", ";
            }
            ostr << ");" << std::endl;
            
            std::string res = function.type().isVoid() ? "R_NilValue" : 
                                                         "Rcpp::wrap(result)";
            ostr << "    return " << res << ";" << std::endl;
            ostr << "END_RCPP" << std::endl;
            ostr << "}" << std::endl;
        }
    }
    
} // namespace attributes
} // namespace Rcpp
