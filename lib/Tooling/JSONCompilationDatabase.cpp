//===--- JSONCompilationDatabase.cpp - ------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file contains the implementation of the JSONCompilationDatabase.
//
//===----------------------------------------------------------------------===//

#include "clang/Tooling/JSONCompilationDatabase.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/CompilationDatabasePluginRegistry.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Path.h"
#include <system_error>

namespace clang {
namespace tooling {

namespace {

/// \brief A parser for escaped strings of command line arguments.
///
/// Assumes \-escaping for quoted arguments (see the documentation of
/// unescapeCommandLine(...)).
class CommandLineArgumentParser {
 public:
  CommandLineArgumentParser(StringRef CommandLine)
      : Input(CommandLine), Position(Input.begin()-1) {}

  std::vector<std::string> parse() {
    bool HasMoreInput = true;
    while (HasMoreInput && nextNonWhitespace()) {
      std::string Argument;
      HasMoreInput = parseStringInto(Argument);
      CommandLine.push_back(Argument);
    }
    return CommandLine;
  }

 private:
  // All private methods return true if there is more input available.

  bool parseStringInto(std::string &String) {
    do {
      if (*Position == '"') {
        if (!parseDoubleQuotedStringInto(String)) return false;
      } else if (*Position == '\'') {
        if (!parseSingleQuotedStringInto(String)) return false;
      } else {
        if (!parseFreeStringInto(String)) return false;
      }
    } while (*Position != ' ');
    return true;
  }

  bool parseDoubleQuotedStringInto(std::string &String) {
    if (!next()) return false;
    while (*Position != '"') {
      if (!skipEscapeCharacter()) return false;
      String.push_back(*Position);
      if (!next()) return false;
    }
    return next();
  }

  bool parseSingleQuotedStringInto(std::string &String) {
    if (!next()) return false;
    while (*Position != '\'') {
      String.push_back(*Position);
      if (!next()) return false;
    }
    return next();
  }

  bool parseFreeStringInto(std::string &String) {
    do {
      if (!skipEscapeCharacter()) return false;
      String.push_back(*Position);
      if (!next()) return false;
    } while (*Position != ' ' && *Position != '"' && *Position != '\'');
    return true;
  }

  bool skipEscapeCharacter() {
    if (*Position == '\\') {
      return next();
    }
    return true;
  }

  bool nextNonWhitespace() {
    do {
      if (!next()) return false;
    } while (*Position == ' ');
    return true;
  }

  bool next() {
    ++Position;
    return Position != Input.end();
  }

  const StringRef Input;
  StringRef::iterator Position;
  std::vector<std::string> CommandLine;
};

std::vector<std::string> unescapeCommandLine(
    StringRef EscapedCommandLine) {
  CommandLineArgumentParser parser(EscapedCommandLine);
  return parser.parse();
}

class JSONCompilationDatabasePlugin : public CompilationDatabasePlugin {
  std::unique_ptr<CompilationDatabase>
  loadFromDirectory(StringRef Directory, std::string &ErrorMessage) override {
    SmallString<1024> JSONDatabasePath(Directory);
    llvm::sys::path::append(JSONDatabasePath, "compile_commands.json");
    std::unique_ptr<CompilationDatabase> Database(
        JSONCompilationDatabase::loadFromFile(JSONDatabasePath, ErrorMessage));
    if (!Database)
      return nullptr;
    return Database;
  }
};

} // end namespace

// Register the JSONCompilationDatabasePlugin with the
// CompilationDatabasePluginRegistry using this statically initialized variable.
static CompilationDatabasePluginRegistry::Add<JSONCompilationDatabasePlugin>
X("json-compilation-database", "Reads JSON formatted compilation databases");

// This anchor is used to force the linker to link in the generated object file
// and thus register the JSONCompilationDatabasePlugin.
volatile int JSONAnchorSource = 0;

std::unique_ptr<JSONCompilationDatabase>
JSONCompilationDatabase::loadFromFile(StringRef FilePath,
                                      std::string &ErrorMessage) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> DatabaseBuffer =
      llvm::MemoryBuffer::getFile(FilePath);
  if (std::error_code Result = DatabaseBuffer.getError()) {
    ErrorMessage = "Error while opening JSON database: " + Result.message();
    return nullptr;
  }
  std::unique_ptr<JSONCompilationDatabase> Database(
      new JSONCompilationDatabase(std::move(*DatabaseBuffer)));
  if (!Database->parse(ErrorMessage))
    return nullptr;
  return Database;
}

std::unique_ptr<JSONCompilationDatabase>
JSONCompilationDatabase::loadFromBuffer(StringRef DatabaseString,
                                        std::string &ErrorMessage) {
  std::unique_ptr<llvm::MemoryBuffer> DatabaseBuffer(
      llvm::MemoryBuffer::getMemBuffer(DatabaseString));
  std::unique_ptr<JSONCompilationDatabase> Database(
      new JSONCompilationDatabase(std::move(DatabaseBuffer)));
  if (!Database->parse(ErrorMessage))
    return nullptr;
  return Database;
}

std::vector<CompileCommand>
JSONCompilationDatabase::getCompileCommands(StringRef FilePath) const {
  SmallString<128> NativeFilePath;
  llvm::sys::path::native(FilePath, NativeFilePath);

  std::string Error;
  llvm::raw_string_ostream ES(Error);
  StringRef Match = MatchTrie.findEquivalent(NativeFilePath, ES);
  if (Match.empty())
    return std::vector<CompileCommand>();
  llvm::StringMap< std::vector<CompileCommandRef> >::const_iterator
    CommandsRefI = IndexByFile.find(Match);
  if (CommandsRefI == IndexByFile.end())
    return std::vector<CompileCommand>();
  std::vector<CompileCommand> Commands;
  getCommands(CommandsRefI->getValue(), Commands);
  return Commands;
}

std::vector<std::string>
JSONCompilationDatabase::getAllFiles() const {
  std::vector<std::string> Result;

  llvm::StringMap< std::vector<CompileCommandRef> >::const_iterator
    CommandsRefI = IndexByFile.begin();
  const llvm::StringMap< std::vector<CompileCommandRef> >::const_iterator
    CommandsRefEnd = IndexByFile.end();
  for (; CommandsRefI != CommandsRefEnd; ++CommandsRefI) {
    Result.push_back(CommandsRefI->first().str());
  }

  return Result;
}

std::vector<CompileCommand>
JSONCompilationDatabase::getAllCompileCommands() const {
  std::vector<CompileCommand> Commands;
  for (llvm::StringMap< std::vector<CompileCommandRef> >::const_iterator
        CommandsRefI = IndexByFile.begin(), CommandsRefEnd = IndexByFile.end();
      CommandsRefI != CommandsRefEnd; ++CommandsRefI) {
    getCommands(CommandsRefI->getValue(), Commands);
  }
  return Commands;
}

void JSONCompilationDatabase::getCommands(
                                  ArrayRef<CompileCommandRef> CommandsRef,
                                  std::vector<CompileCommand> &Commands) const {
  for (int I = 0, E = CommandsRef.size(); I != E; ++I) {
    SmallString<8> DirectoryStorage;
    SmallString<1024> CommandStorage;
    Commands.emplace_back(
      CommandsRef[I].first->getValue(DirectoryStorage),
      CommandsRef[I].second);
  }
}

bool JSONCompilationDatabase::parse(std::string &ErrorMessage) {
  llvm::yaml::document_iterator I = YAMLStream.begin();
  if (I == YAMLStream.end()) {
    ErrorMessage = "Error while parsing YAML.";
    return false;
  }
  llvm::yaml::Node *Root = I->getRoot();
  if (!Root) {
    ErrorMessage = "Error while parsing YAML.";
    return false;
  }
  llvm::yaml::SequenceNode *Array = dyn_cast<llvm::yaml::SequenceNode>(Root);
  if (!Array) {
    ErrorMessage = "Expected array.";
    return false;
  }
  for (auto& NextObject : *Array) {
    llvm::yaml::MappingNode *Object = dyn_cast<llvm::yaml::MappingNode>(&NextObject);
    if (!Object) {
      ErrorMessage = "Expected object.";
      return false;
    }
    llvm::yaml::ScalarNode *Directory = nullptr;
    std::vector<std::string> Arguments;
    std::vector<std::string> Command;
    llvm::yaml::ScalarNode *File = nullptr;
    bool ArgumentsFound = false;
    bool CommandFound = false;
    for (auto& NextKeyValue : *Object) {
      llvm::yaml::ScalarNode *KeyString =
          dyn_cast<llvm::yaml::ScalarNode>(NextKeyValue.getKey());
      if (!KeyString) {
        ErrorMessage = "Expected strings as key.";
        return false;
      }
      SmallString<10> KeyStorage;
      StringRef KeyValue = KeyString->getValue(KeyStorage);
      llvm::yaml::Node *Value = NextKeyValue.getValue();
      if (!Value) {
        ErrorMessage = "Expected value.";
        return false;
      }
      llvm::yaml::ScalarNode *ValueString =
          dyn_cast<llvm::yaml::ScalarNode>(Value);
      llvm::yaml::SequenceNode *SequenceString =
          dyn_cast<llvm::yaml::SequenceNode>(Value);
      if (KeyValue == "arguments" && !SequenceString) {
        ErrorMessage = "Expected sequence as value.";
        return false;
      } else if (KeyValue != "arguments" && !ValueString) {
        ErrorMessage = "Expected string as value.";
        return false;
      }
      if (KeyValue == "directory") {
        Directory = ValueString;
      } else if (KeyValue == "arguments") {
        for (auto& NextArgument : *SequenceString) {
          SmallString<128> CommandStorage;
          auto ValueString = dyn_cast<llvm::yaml::ScalarNode>(&NextArgument);

          Arguments.push_back(ValueString->getValue(CommandStorage));
        }
        ArgumentsFound = true;
      } else if (KeyValue == "command") {
        SmallString<1024> CommandStorage;
        // FIXME: Escape correctly:
        Command = unescapeCommandLine(ValueString->getValue(CommandStorage));
        CommandFound = true;
      } else if (KeyValue == "file") {
        File = ValueString;
      } else {
        ErrorMessage = ("Unknown key: \"" +
                        KeyString->getRawValue() + "\"").str();
        return false;
      }
    }
    if (!File) {
      ErrorMessage = "Missing key: \"file\".";
      return false;
    }
    if (!ArgumentsFound && !CommandFound) {
      ErrorMessage = "Missing key: \"command\" or \"arguments\".";
      return false;
    }
    if (!Directory) {
      ErrorMessage = "Missing key: \"directory\".";
      return false;
    }
    SmallString<8> FileStorage;
    StringRef FileName = File->getValue(FileStorage);
    SmallString<128> NativeFilePath;
    if (llvm::sys::path::is_relative(FileName)) {
      SmallString<8> DirectoryStorage;
      SmallString<128> AbsolutePath(
          Directory->getValue(DirectoryStorage));
      llvm::sys::path::append(AbsolutePath, FileName);
      llvm::sys::path::native(AbsolutePath, NativeFilePath);
    } else {
      llvm::sys::path::native(FileName, NativeFilePath);
    }
    IndexByFile[NativeFilePath].push_back(
        CompileCommandRef(Directory, ArgumentsFound ? Arguments : Command));
    MatchTrie.insert(NativeFilePath);
  }
  return true;
}

} // end namespace tooling
} // end namespace clang
