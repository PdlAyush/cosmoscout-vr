////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
////////////////////////////////////////////////////////////////////////////////////////////////////

// SPDX-FileCopyrightText: German Aerospace Center (DLR) <cosmoscout@dlr.de>
// SPDX-License-Identifier: MIT

#include "NodeFactory.hpp"

#include "logger.hpp"

namespace csl::nodeeditor {

////////////////////////////////////////////////////////////////////////////////////////////////////

void NodeFactory::registerSocketType(
    std::string const& name, std::string color, std::vector<std::string> compatibleTo) {
  mSockets[name] = {std::move(color), std::move(compatibleTo)};
}

////////////////////////////////////////////////////////////////////////////////////////////////////

std::string NodeFactory::getSocketSource() const {
  std::string source;

  for (auto const& s : mSockets) {
    source += fmt::format("SOCKETS['{0}'] = new Rete.Socket('{0}');\n", s.first);
  }

  for (auto const& s : mSockets) {
    source += fmt::format("addSocketStyle('{}', '{}');\n", s.first, s.second.mColor);
  }

  for (auto const& s : mSockets) {
    for (auto const& o : s.second.mCompatibleTo) {
      source += fmt::format("SOCKETS['{}'].combineWith(SOCKETS['{}']);\n", s.first, o);
    }
  }

  return source;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

std::string NodeFactory::getNodeSource() const {
  std::string source;

  for (auto const& f : mNodeSourceFuncs) {
    source += f();
  }

  return source;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

std::string NodeFactory::getRegisterSource() const {
  std::string source;

  for (auto const& f : mNodeCreateFuncs) {
    source += "{\n";
    source += fmt::format("const component = new {}Component();\n", f.first);
    source += "editor.register(component);\n";
    source += "engine.register(component);\n";
    source += "}\n";
  }

  return source;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace csl::nodeeditor
