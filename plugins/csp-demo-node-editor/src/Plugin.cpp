////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
////////////////////////////////////////////////////////////////////////////////////////////////////

// SPDX-FileCopyrightText: German Aerospace Center (DLR) <cosmoscout@dlr.de>
// SPDX-License-Identifier: MIT

#include "Plugin.hpp"

#include "../../../src/cs-core/GuiManager.hpp"
#include "../../../src/cs-core/Settings.hpp"
#include "../../../src/cs-core/SolarSystem.hpp"
#include "../../../src/cs-utils/logger.hpp"
#include "../../../src/cs-utils/utils.hpp"

#include "logger.hpp"
#include "nodes/DisplayNode.hpp"
#include "nodes/MathNode.hpp"
#include "nodes/NumberNode.hpp"
#include "nodes/TimeNode.hpp"

////////////////////////////////////////////////////////////////////////////////////////////////////

EXPORT_FN cs::core::PluginBase* create() {
  return new csp::demonodeeditor::Plugin;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

EXPORT_FN void destroy(cs::core::PluginBase* pluginBase) {
  delete pluginBase; // NOLINT(cppcoreguidelines-owning-memory)
}

////////////////////////////////////////////////////////////////////////////////////////////////////

namespace csp::demonodeeditor {

////////////////////////////////////////////////////////////////////////////////////////////////////

void from_json(nlohmann::json const& j, Plugin::Settings& o) {
  cs::core::Settings::deserialize(j, "port", o.mPort);
  cs::core::Settings::deserialize(j, "graph", o.mGraph);
}

void to_json(nlohmann::json& j, Plugin::Settings const& o) {
  cs::core::Settings::serialize(j, "port", o.mPort);
  cs::core::Settings::serialize(j, "graph", o.mGraph);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::init() {

  logger().info("Loading plugin...");

  mOnLoadConnection = mAllSettings->onLoad().connect([this]() { onLoad(); });
  mOnSaveConnection = mAllSettings->onSave().connect([this]() { onSave(); });

  // Restart the server if the port changes.
  mPluginSettings.mPort.connect([this](uint16_t port) { setupNodeEditor(port); });

  onLoad();

  logger().info("Loading done.");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::deInit() {
  logger().info("Unloading plugin...");

  // Save settings as this plugin may get reloaded.
  onSave();

  mAllSettings->onLoad().disconnect(mOnLoadConnection);
  mAllSettings->onSave().disconnect(mOnSaveConnection);

  mNodeEditor.reset();

  logger().info("Unloading done.");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::update() {
  mNodeEditor->update();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::onLoad() {
  from_json(mAllSettings->mPlugins.at("csp-demo-node-editor"), mPluginSettings);

  if (mPluginSettings.mGraph.has_value()) {
    try {
      mNodeEditor->fromJSON(mPluginSettings.mGraph.value());
    } catch (std::exception const& e) { logger().warn("Failed to load node graph: {}", e.what()); }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::onSave() {
  mPluginSettings.mGraph = mNodeEditor->toJSON();

  mAllSettings->mPlugins["csp-demo-node-editor"] = mPluginSettings;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::setupNodeEditor(uint16_t port) {

  csl::nodeeditor::NodeFactory factory;
  factory.registerSocketType("Number Value", "#b08ab3");
  factory.registerSocketType("Date Value", "#00ff00");

  factory.registerNodeType<DisplayNode>();
  factory.registerNodeType<NumberNode>();
  factory.registerNodeType<MathNode>();
  factory.registerNodeType<TimeNode>(mTimeControl);

  mNodeEditor = std::make_unique<csl::nodeeditor::NodeEditor>(port, factory);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace csp::demonodeeditor
