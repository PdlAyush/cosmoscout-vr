////////////////////////////////////////////////////////////////////////////////////////////////////
//                               This file is part of CosmoScout VR                               //
//      and may be used under the terms of the MIT license. See the LICENSE file for details.     //
//                        Copyright: (c) 2019 German Aerospace Center (DLR)                       //
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "Plugin.hpp"
#include "TextureOverlayRenderer.hpp"
#include "WebMapService.hpp"
#include "logger.hpp"

#include "../../../src/cs-core/GuiManager.hpp"
#include "../../../src/cs-core/InputManager.hpp"
#include "../../../src/cs-core/Settings.hpp"
#include "../../../src/cs-core/SolarSystem.hpp"
#include "../../../src/cs-core/TimeControl.hpp"

#include <VistaKernel/DisplayManager/VistaDisplayManager.h>
#include <VistaKernel/VistaSystem.h>

////////////////////////////////////////////////////////////////////////////////////////////////////

EXPORT_FN cs::core::PluginBase* create() {
  return new csp::wmsoverlays::Plugin;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

EXPORT_FN void destroy(cs::core::PluginBase* pluginBase) {
  delete pluginBase; // NOLINT(cppcoreguidelines-owning-memory)
}

////////////////////////////////////////////////////////////////////////////////////////////////////

namespace csp::wmsoverlays {

////////////////////////////////////////////////////////////////////////////////////////////////////

void from_json(nlohmann::json const& j, Bounds& o) {
  std::array<double, 4> bounds;
  j.get_to(bounds);
  o.mMinLon = bounds[0];
  o.mMaxLon = bounds[1];
  o.mMinLat = bounds[2];
  o.mMaxLat = bounds[3];
}

void to_json(nlohmann::json& j, Bounds const& o) {
  std::array<double, 4> bounds{o.mMinLon, o.mMaxLon, o.mMinLat, o.mMaxLat};
  j = bounds;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void from_json(nlohmann::json const& j, Plugin::Settings::Body& o) {
  cs::core::Settings::deserialize(j, "activeServer", o.mActiveServer);
  cs::core::Settings::deserialize(j, "activeLayer", o.mActiveLayer);
  cs::core::Settings::deserialize(j, "activeStyle", o.mActiveStyle);
  cs::core::Settings::deserialize(j, "activeBounds", o.mActiveBounds);
  cs::core::Settings::deserialize(j, "wms", o.mWms);
}

void to_json(nlohmann::json& j, Plugin::Settings::Body const& o) {
  cs::core::Settings::serialize(j, "activeServer", o.mActiveServer);
  cs::core::Settings::serialize(j, "activeLayer", o.mActiveLayer);
  cs::core::Settings::serialize(j, "activeStyle", o.mActiveStyle);
  cs::core::Settings::serialize(j, "activeBounds", o.mActiveBounds);
  cs::core::Settings::serialize(j, "wms", o.mWms);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void from_json(nlohmann::json const& j, Plugin::Settings& o) {
  cs::core::Settings::deserialize(j, "preFetch", o.mPrefetchCount);
  cs::core::Settings::deserialize(j, "maxTextureSize", o.mMaxTextureSize);
  cs::core::Settings::deserialize(j, "mapCache", o.mMapCache);
  cs::core::Settings::deserialize(j, "capabilityCache", o.mCapabilityCache);
  cs::core::Settings::deserialize(j, "bodies", o.mBodies);
}

void to_json(nlohmann::json& j, Plugin::Settings const& o) {
  cs::core::Settings::serialize(j, "preFetch", o.mPrefetchCount);
  cs::core::Settings::serialize(j, "maxTextureSize", o.mMaxTextureSize);
  cs::core::Settings::serialize(j, "mapCache", o.mMapCache);
  cs::core::Settings::serialize(j, "capabilityCache", o.mCapabilityCache);
  cs::core::Settings::serialize(j, "bodies", o.mBodies);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::init() {
  logger().info("Loading plugin...");

  mOnLoadConnection = mAllSettings->onLoad().connect([this]() { onLoad(); });

  mOnSaveConnection = mAllSettings->onSave().connect(
      [this]() { mAllSettings->mPlugins["csp-wms-overlays"] = *mPluginSettings; });

  mGuiManager->addPluginTabToSideBarFromHTML(
      "WMS Overlays", "panorama", "../share/resources/gui/wms_overlays_tab.html");
  mGuiManager->addSettingsSectionToSideBarFromHTML(
      "WMS Overlays", "panorama", "../share/resources/gui/wms_settings.html");
  mGuiManager->addScriptToGuiFromJS("../share/resources/gui/js/csp-wms-overlays.js");
  mGuiManager->addCssToGui("css/csp-wms-overlays.css");

  mPluginSettings->mMaxTextureSize.connect([this](int value) {
    if (!mActiveOverlay || !mActiveLayers[mActiveOverlay->getCenter()]) {
      return;
    }
    checkScale(
        mActiveOverlay->pBounds.get(), mActiveLayers[mActiveOverlay->getCenter()].value(), value);
  });

  // Updates the bounds for which map data is requested.
  mGuiManager->getGui()->registerCallback(
      "wmsOverlays.updateBounds", "Updates the bounds for map requests.", std::function([this]() {
        if (mActiveOverlay) {
          mActiveOverlay->requestUpdateBounds();
        }
      }));

  // Resets the bounds to the default ones for the active layer.
  mGuiManager->getGui()->registerCallback(
      "wmsOverlays.resetBounds", "Resets the bounds for map requests.", std::function([this]() {
        if (mActiveOverlay && mActiveLayers[mActiveOverlay->getCenter()]) {
          mActiveOverlay->pBounds =
              mActiveLayers[mActiveOverlay->getCenter()]->getSettings().mBounds;
        }
      }));

  // Moves the observer to a position from which most of the current layer should be visible.
  mGuiManager->getGui()->registerCallback("wmsOverlays.goToDefaultBounds",
      "Fly the observer to the center of the default bounds of the current layer.",
      std::function([this]() {
        if (!mActiveOverlay || !mActiveLayers[mActiveOverlay->getCenter()]) {
          return;
        }
        WebMapLayer::Settings layerSettings =
            mActiveLayers[mActiveOverlay->getCenter()]->getSettings();

        goToBounds(layerSettings.mBounds);
      }));

  // Moves the observer to a position from which most of the currently set bounds should be visible.
  mGuiManager->getGui()->registerCallback("wmsOverlays.goToCurrentBounds",
      "Fly the observer to the center of the current bounds.", std::function([this]() {
        if (!mActiveOverlay) {
          return;
        }

        goToBounds(mActiveOverlay->pBounds.get());
      }));

  mGuiManager->getGui()->registerCallback(
      "wmsOverlays.showInfo", "Toggles the info window.", std::function([this]() {
        mGuiManager->getGui()->executeJavascript(
            "document.getElementById('wmsOverlays.infoWindow').classList.toggle('visible')");
      }));

  // Set whether to interpolate textures between timesteps (does not work when pre-fetch is
  // inactive).
  mGuiManager->getGui()->registerCallback("wmsOverlays.setEnableTimeInterpolation",
      "Enables or disables interpolation.",
      std::function([this](bool enable) { mPluginSettings->mEnableInterpolation = enable; }));

  // Set whether to automatically update bounds.
  mGuiManager->getGui()->registerCallback("wmsOverlays.setEnableAutomaticBoundsUpdate",
      "Enables or disables automatic bounds update.", std::function([this](bool enable) {
        mPluginSettings->mEnableAutomaticBoundsUpdate = enable;
      }));

  // Set maximume texture size for map requests.
  mGuiManager->getGui()->registerCallback("wmsOverlays.setMaxTextureSize",
      "Set the maximum texture size.", std::function([this](double value) {
        mPluginSettings->mMaxTextureSize = std::lround(value);
      }));

  mGuiManager->getGui()->registerCallback("wmsOverlays.setPrefetchCount",
      "Set the amount of images to prefetch.", std::function([this](double value) {
        mPluginSettings->mPrefetchCount = std::lround(value);
      }));

  mGuiManager->getGui()->registerCallback("wmsOverlays.setUpdateBoundsDelay",
      "Set the delay that has to pass before an automatic bounds update.",
      std::function(
          [this](double value) { mPluginSettings->mUpdateBoundsDelay = std::lround(value); }));

  // Set WMS source.
  mGuiManager->getGui()->registerCallback("wmsOverlays.setServer",
      "Set the current planet's WMS server to the one with the given name.",
      std::function([this](std::string&& name) {
        if (mActiveOverlay) {
          setWMSServer(mActiveOverlay, name);
          mNoMovementRequestedUpdate = false;
        }
      }));

  mGuiManager->getGui()->registerCallback("wmsOverlays.setLayer",
      "Set the current planet's WMS layer to the one with the given name.",
      std::function([this](std::string&& name) {
        if (mActiveOverlay && mActiveServers[mActiveOverlay->getCenter()]) {
          setWMSLayer(mActiveOverlay, name);
          mNoMovementRequestedUpdate = false;
        }
      }));

  mGuiManager->getGui()->registerCallback("wmsOverlays.setStyle",
      "Sets the style for the currently selected layer.", std::function([this](std::string&& name) {
        if (mActiveOverlay || mActiveLayers[mActiveOverlay->getCenter()]) {
          setWMSStyle(mActiveOverlay, name);
          mNoMovementRequestedUpdate = false;
        }
      }));

  mGuiManager->getGui()->registerCallback(
      "wmsOverlays.goToFirstTime", "Go to the first available timestep.", std::function([this]() {
        if (!mActiveOverlay || !mActiveLayers[mActiveOverlay->getCenter()] ||
            mActiveLayers[mActiveOverlay->getCenter()]->getSettings().mTimeIntervals.empty()) {
          return;
        }
        mTimeControl->setTimeSpeed(0);
        mTimeControl->setTime(
            cs::utils::convert::time::toSpice(mActiveLayers[mActiveOverlay->getCenter()]
                                                  ->getSettings()
                                                  .mTimeIntervals.front()
                                                  .mStartTime));
      }));

  mGuiManager->getGui()->registerCallback("wmsOverlays.goToPreviousTime",
      "Go to the previous available timestep.", std::function([this]() {
        if (!mActiveOverlay || !mActiveLayers[mActiveOverlay->getCenter()] ||
            mActiveLayers[mActiveOverlay->getCenter()]->getSettings().mTimeIntervals.empty()) {
          return;
        }

        mTimeControl->setTimeSpeed(0);

        boost::posix_time::ptime time =
            cs::utils::convert::time::toPosix(mTimeControl->pSimulationTime.get());

        std::vector<TimeInterval> intervals =
            mActiveLayers[mActiveOverlay->getCenter()]->getSettings().mTimeIntervals;

        // Check if current time is in any interval
        TimeInterval             result;
        boost::posix_time::ptime sampleStartTime = time;
        if (utils::timeInIntervals(sampleStartTime, intervals, result)) {
          if (sampleStartTime != time) {
            // timeInIntervals rounds down the time to the nearest timestep, so the
            // result of that method can be used.
            mTimeControl->setTime(cs::utils::convert::time::toSpice(sampleStartTime));
            return;
          } else {
            // The current time was a valid timestep so the previous step has to be found.
            if (sampleStartTime == result.mStartTime) {
              auto it = std::find(intervals.begin(), intervals.end(), result);
              if (it == intervals.begin()) {
                // If the time is at the start of the first interval, there is no previous
                // timestep to go to.
                return;
              } else {
                // If the time is at the start of another interval, the previous timestep is the
                // end time of the previous interval.
                // Currently we trust that the intervals are ordered chronologically
                mTimeControl->setTime(cs::utils::convert::time::toSpice((it - 1)->mEndTime));
                return;
              }
            }
            // If the time was not the start time of any interval we can substract the duration to
            // get the previous timestep.
            sampleStartTime = utils::addDurationToTime(sampleStartTime, result.mSampleDuration, -1);
            mTimeControl->setTime(cs::utils::convert::time::toSpice(sampleStartTime));
            return;
          }
        }

        boost::posix_time::ptime temp = time;
        for (auto const& interval : intervals) {
          if (time > interval.mEndTime) {
            temp = interval.mEndTime;
          } else if (time < interval.mStartTime) {
            mTimeControl->setTime(cs::utils::convert::time::toSpice(temp));
            return;
          }
        }
        mTimeControl->setTime(cs::utils::convert::time::toSpice(temp));
      }));

  mGuiManager->getGui()->registerCallback(
      "wmsOverlays.goToNextTime", "Go to the next available timestep.", std::function([this]() {
        if (!mActiveOverlay || !mActiveLayers[mActiveOverlay->getCenter()] ||
            mActiveLayers[mActiveOverlay->getCenter()]->getSettings().mTimeIntervals.empty()) {
          return;
        }

        mTimeControl->setTimeSpeed(0);

        boost::posix_time::ptime time =
            cs::utils::convert::time::toPosix(mTimeControl->pSimulationTime.get());

        std::vector<TimeInterval> intervals =
            mActiveLayers[mActiveOverlay->getCenter()]->getSettings().mTimeIntervals;

        // Check if current time is in any interval
        TimeInterval             result;
        boost::posix_time::ptime sampleStartTime = time;
        if (utils::timeInIntervals(sampleStartTime, intervals, result)) {
          if (sampleStartTime == result.mEndTime) {
            auto it = std::find(intervals.begin(), intervals.end(), result);
            if (it == intervals.end() - 1) {
              // If the time is at the end of the last interval, there is no next
              // timestep to go to.
              return;
            } else {
              // If the time is at the end of another interval, the next timestep is the
              // start time of the next interval.
              // Currently we trust that the intervals are ordered chronologically
              mTimeControl->setTime(cs::utils::convert::time::toSpice((it + 1)->mStartTime));
              return;
            }
          }
          // If the time was not the end time of any interval we can add the duration to
          // get the next timestep.
          sampleStartTime = utils::addDurationToTime(sampleStartTime, result.mSampleDuration);
          mTimeControl->setTime(cs::utils::convert::time::toSpice(sampleStartTime));
          return;
        }

        for (auto const& interval : intervals) {
          if (time < interval.mStartTime) {
            mTimeControl->setTime(cs::utils::convert::time::toSpice(interval.mStartTime));
            return;
          }
        }
      }));

  mGuiManager->getGui()->registerCallback(
      "wmsOverlays.goToLastTime", "Go to the last available timestep.", std::function([this]() {
        if (!mActiveOverlay || !mActiveLayers[mActiveOverlay->getCenter()] ||
            mActiveLayers[mActiveOverlay->getCenter()]->getSettings().mTimeIntervals.empty()) {
          return;
        }
        mTimeControl->setTimeSpeed(0);
        mTimeControl->setTime(
            cs::utils::convert::time::toSpice(mActiveLayers[mActiveOverlay->getCenter()]
                                                  ->getSettings()
                                                  .mTimeIntervals.back()
                                                  .mEndTime));
      }));

  mActiveBodyConnection = mSolarSystem->pActiveBody.connectAndTouch(
      [this](std::shared_ptr<cs::scene::CelestialBody> const& body) {
        if (!body) {
          return;
        }

        auto overlay = mWMSOverlays.find(body->getCenterName());

        mGuiManager->getGui()->callJavascript(
            "CosmoScout.sidebar.setTabEnabled", "WMS Overlays", overlay != mWMSOverlays.end());

        if (overlay == mWMSOverlays.end()) {
          mActiveOverlay = nullptr;
          return;
        }

        if (mActiveOverlay) {
          mActiveOverlay->pBounds.disconnect(mBoundsConnection);
        }
        mActiveOverlay    = overlay->second;
        mBoundsConnection = mActiveOverlay->pBounds.connectAndTouch([this](Bounds bounds) {
          mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.setCurrentBounds",
              bounds.mMinLon, bounds.mMaxLon, bounds.mMinLat, bounds.mMaxLat);

          if (!mActiveOverlay || !mActiveLayers[mActiveOverlay->getCenter()]) {
            return;
          }
          checkScale(bounds, mActiveLayers[mActiveOverlay->getCenter()].value(),
              mPluginSettings->mMaxTextureSize.get());
        });

        mGuiManager->getGui()->callJavascript(
            "CosmoScout.gui.clearDropdown", "wmsOverlays.setServer");
        mGuiManager->getGui()->callJavascript(
            "CosmoScout.gui.addDropdownValue", "wmsOverlays.setServer", "None", "None", false);

        auto const& settings   = getBodySettings(overlay->second);
        bool        noneActive = true;
        for (auto const& server : mWms[body->getCenterName()]) {
          bool active = server.getTitle() == settings.mActiveServer.get();
          mGuiManager->getGui()->callJavascript("CosmoScout.gui.addDropdownValue",
              "wmsOverlays.setServer", server.getTitle(), server.getTitle(), active);

          if (active) {
            noneActive = false;
            setWMSServer(overlay->second, server.getTitle());
          }
        }

        if (noneActive) {
          resetWMSServer(overlay->second);
        }
      });

  mObserverSpeedConnection = mSolarSystem->pCurrentObserverSpeed.connect([this](float speed) {
    if (speed == 0.f) {
      mNoMovementSince           = std::chrono::high_resolution_clock::now();
      mNoMovement                = true;
      mNoMovementRequestedUpdate = false;
    } else {
      mNoMovement = false;
    }
  });

  onLoad();

  logger().info("Loading done.");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::deInit() {
  logger().info("Unloading plugin...");

  mSolarSystem->pActiveBody.disconnect(mActiveBodyConnection);
  mSolarSystem->pCurrentObserverSpeed.disconnect(mObserverSpeedConnection);

  mGuiManager->removePluginTab("WMS Overlays");
  mGuiManager->removeSettingsSection("WMS Overlays");

  mGuiManager->getGui()->callJavascript(
      "CosmoScout.gui.unregisterCss", "css/csp-simple-wms-bodies.css");

  mGuiManager->getGui()->unregisterCallback("wmsOverlays.setEnableTimeInterpolation");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.setEnableAutomaticBoundsUpdate");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.setMaxTextureSize");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.setPrefetchCount");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.setUpdateBoundsDelay");

  mGuiManager->getGui()->unregisterCallback("wmsOverlays.showInfo");

  mGuiManager->getGui()->unregisterCallback("wmsOverlays.setServer");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.setLayer");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.setStyle");

  mGuiManager->getGui()->unregisterCallback("wmsOverlays.goToFirstTime");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.goToPreviousTime");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.goToNextTime");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.goToLastTime");

  mGuiManager->getGui()->unregisterCallback("wmsOverlays.updateBounds");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.resetBounds");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.goToDefaultBounds");
  mGuiManager->getGui()->unregisterCallback("wmsOverlays.goToCurrentBounds");

  mAllSettings->onLoad().disconnect(mOnLoadConnection);
  mAllSettings->onSave().disconnect(mOnSaveConnection);

  logger().info("Unloading done.");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::update() {
  if (mPluginSettings->mEnableAutomaticBoundsUpdate.get() && mNoMovement &&
      !mNoMovementRequestedUpdate &&
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::high_resolution_clock::now() - mNoMovementSince)
              .count() >= mPluginSettings->mUpdateBoundsDelay.get()) {
    mNoMovementRequestedUpdate = true;

    if (mActiveOverlay) {
      mActiveOverlay->requestUpdateBounds();
    }
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::onLoad() {
  // Read settings from JSON.
  from_json(mAllSettings->mPlugins.at("csp-wms-overlays"), *mPluginSettings);

  // First try to re-configure existing WMS overlays. We assume that they are similar if they
  // have the same name in the settings (which means they are attached to an anchor with the same
  // name).
  auto wmsOverlay = mWMSOverlays.begin();
  while (wmsOverlay != mWMSOverlays.end()) {
    auto settings = mPluginSettings->mBodies.find(wmsOverlay->first);
    if (settings != mPluginSettings->mBodies.end()) {
      if (!settings->second.mActiveServer.isDefault()) {
        setWMSServer(wmsOverlay->second, settings->second.mActiveServer.get());
      } else {
        resetWMSServer(wmsOverlay->second);
      }

      // If there are settings for this simpleWMSBody, reconfigure it.
      wmsOverlay->second->configure(settings->second);

      ++wmsOverlay;
    } else {
      // Else delete it.
      wmsOverlay = mWMSOverlays.erase(wmsOverlay);
    }
  }

  // Then add new WMS overlays.
  for (auto& settings : mPluginSettings->mBodies) {
    if (mWMSOverlays.find(settings.first) != mWMSOverlays.end()) {
      continue;
    }

    auto anchor = mAllSettings->mAnchors.find(settings.first);

    if (anchor == mAllSettings->mAnchors.end()) {
      throw std::runtime_error(
          "There is no Anchor \"" + settings.first + "\" defined in the settings.");
    }

    auto wmsOverlay = std::make_shared<TextureOverlayRenderer>(
        settings.first, mSolarSystem, mTimeControl, mAllSettings, mPluginSettings);

    mWMSOverlays.emplace(settings.first, wmsOverlay);

    for (auto const& wmsUrl : settings.second.mWms) {
      try {
        mWms[settings.first].emplace_back(wmsUrl, mPluginSettings->mCapabilityCache.get());
      } catch (std::exception const& e) {
        logger().warn("Failed to parse capabilities for '{}': '{}'!", wmsUrl, e.what());
      }
    }

    if (!settings.second.mActiveServer.isDefault()) {
      setWMSServer(wmsOverlay, settings.second.mActiveServer.get());
    } else {
      resetWMSServer(wmsOverlay);
    }

    wmsOverlay->configure(settings.second);

    wmsOverlay->pBounds.connectAndTouch([this, &settings, center = settings.first](Bounds bounds) {
      settings.second.mActiveBounds = bounds;
      if (isActiveOverlay(center)) {
        mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.setCurrentBounds",
            bounds.mMinLon, bounds.mMaxLon, bounds.mMinLat, bounds.mMaxLat);
        checkScale(bounds, mActiveLayers[center].value(), mPluginSettings->mMaxTextureSize.get());
      }
    });
  }

  mSolarSystem->pActiveBody.touch(mActiveBodyConnection);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

Plugin::Settings::Body& Plugin::getBodySettings(
    std::shared_ptr<TextureOverlayRenderer> const& wmsOverlay) const {
  auto name = std::find_if(mWMSOverlays.begin(), mWMSOverlays.end(),
      [&](auto const& pair) { return pair.second == wmsOverlay; });
  return mPluginSettings->mBodies.at(name->first);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::setWMSServer(
    std::shared_ptr<TextureOverlayRenderer> const& wmsOverlay, std::string const& name) {
  auto&       settings = getBodySettings(wmsOverlay);
  auto const& server =
      std::find_if(mWms.at(wmsOverlay->getCenter()).begin(), mWms.at(wmsOverlay->getCenter()).end(),
          [&name](WebMapService wms) { return wms.getTitle() == name; });

  if (server == mWms.at(wmsOverlay->getCenter()).end()) {
    if (name != "None") {
      logger().warn("No server with name '{}' found!", name);
    }
    resetWMSServer(wmsOverlay);
    return;
  }

  settings.mActiveServer = name;
  mActiveServers[wmsOverlay->getCenter()].emplace(*server);

  if (isActiveOverlay(wmsOverlay)) {
    mGuiManager->getGui()->callJavascript(
        "CosmoScout.gui.setDropdownValue", "wmsOverlays.setServer", server->getTitle(), false);
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.resetLayerSelect");
  }

  bool        noneActive = true;
  WebMapLayer root       = server->getRootLayer();
  for (auto const& layer : root.getAllLayers()) {
    if (addLayerToSelect(wmsOverlay, layer, settings.mActiveLayer.get())) {
      noneActive = false;
    }
  }
  if (isActiveOverlay(wmsOverlay)) {
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.refreshLayerSelect");
  }
  if (noneActive) {
    resetWMSLayer(wmsOverlay);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::resetWMSServer(std::shared_ptr<TextureOverlayRenderer> const& wmsOverlay) {
  if (isActiveOverlay(wmsOverlay)) {
    mGuiManager->getGui()->callJavascript(
        "CosmoScout.gui.setDropdownValue", "wmsOverlays.setServer", "None", false);
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.resetLayerSelect");
  }

  auto& settings = getBodySettings(wmsOverlay);
  settings.mActiveServer.reset();
  mActiveServers[wmsOverlay->getCenter()].reset();
  resetWMSLayer(wmsOverlay);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::setWMSLayer(
    std::shared_ptr<TextureOverlayRenderer> const& wmsOverlay, std::string const& name) {
  auto&                      settings = getBodySettings(wmsOverlay);
  std::optional<WebMapLayer> layer    = mActiveServers[wmsOverlay->getCenter()]->getLayer(name);

  if (!layer.has_value()) {
    if (name != "None") {
      logger().warn("Can't set layer '{}': No such layer found for server '{}'", name,
          mActiveServers[wmsOverlay->getCenter()]->getTitle());
    }
    resetWMSLayer(wmsOverlay);
    return;
  }

  settings.mActiveLayer = name;
  mActiveLayers[wmsOverlay->getCenter()].emplace(layer.value());
  wmsOverlay->setActiveWMS(mActiveServers[wmsOverlay->getCenter()].value(),
      mActiveLayers[wmsOverlay->getCenter()].value());

  if (isActiveOverlay(wmsOverlay)) {
    mGuiManager->getGui()->callJavascript(
        "CosmoScout.gui.setDropdownValue", "wmsOverlays.setLayer", layer->getName(), false);

    mGuiManager->getGui()->callJavascript("CosmoScout.gui.clearDropdown", "wmsOverlays.setStyle");
    mGuiManager->getGui()->callJavascript(
        "CosmoScout.gui.addDropdownValue", "wmsOverlays.setStyle", "", "Default", false);

    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.setInfo", layer->getTitle(),
        boost::replace_all_copy(
            layer->getAbstract().value_or("<em>No description given</em>"), "\r", "</br>"),
        layer->getSettings().mAttribution.value_or("None"));
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.enableInfoButton", true);

    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.enableTimeNavigation",
        !layer->getSettings().mTimeIntervals.empty());
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.setDefaultBounds",
        layer->getSettings().mBounds.mMinLon, layer->getSettings().mBounds.mMaxLon,
        layer->getSettings().mBounds.mMinLat, layer->getSettings().mBounds.mMaxLat);

    if (layer->getSettings().mNoSubsets) {
      mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.setNoSubsets");
    } else {
      mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.setCurrentBounds",
          wmsOverlay->pBounds.get().mMinLon, wmsOverlay->pBounds.get().mMaxLon,
          wmsOverlay->pBounds.get().mMinLat, wmsOverlay->pBounds.get().mMaxLat);
    }

    checkScale(wmsOverlay->pBounds.get(), layer.value(), mPluginSettings->mMaxTextureSize.get());
  }

  bool noneActive = true;
  for (WebMapLayer::Style style : layer->getSettings().mStyles) {
    bool active = style.mName == settings.mActiveStyle.get();
    if (isActiveOverlay(wmsOverlay)) {
      mGuiManager->getGui()->callJavascript("CosmoScout.gui.addDropdownValue",
          "wmsOverlays.setStyle", style.mName, style.mTitle, active);
    }

    if (active) {
      noneActive = false;
      setWMSStyle(wmsOverlay, style.mName);
    }
  }
  if (noneActive) {
    resetWMSStyle(wmsOverlay);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::resetWMSLayer(std::shared_ptr<TextureOverlayRenderer> const& wmsOverlay) {
  if (isActiveOverlay(wmsOverlay)) {
    mGuiManager->getGui()->callJavascript(
        "CosmoScout.gui.setDropdownValue", "wmsOverlays.setLayer", "None", false);

    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.enableInfoButton", false);
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.clearDefaultBounds");
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.clearCurrentBounds");
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.enableTimeNavigation", false);

    mGuiManager->getGui()->callJavascript("CosmoScout.gui.clearDropdown", "wmsOverlays.setStyle");
    mGuiManager->getGui()->callJavascript(
        "CosmoScout.gui.addDropdownValue", "wmsOverlays.setStyle", "", "Default", false);
  }

  auto& settings = getBodySettings(wmsOverlay);
  settings.mActiveLayer.reset();
  mActiveLayers[wmsOverlay->getCenter()].reset();
  wmsOverlay->clearActiveWMS();
  resetWMSStyle(wmsOverlay);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::setWMSStyle(
    std::shared_ptr<TextureOverlayRenderer> const& wmsOverlay, std::string const& name) {
  auto                  bodySettings  = getBodySettings(wmsOverlay);
  WebMapLayer::Settings layerSettings = mActiveLayers[wmsOverlay->getCenter()]->getSettings();
  auto const& style = std::find_if(layerSettings.mStyles.begin(), layerSettings.mStyles.end(),
      [&name](WebMapLayer::Style style) { return style.mName == name; });

  if (style != layerSettings.mStyles.end()) {
    if (isActiveOverlay(wmsOverlay)) {
      mGuiManager->getGui()->callJavascript(
          "CosmoScout.gui.setDropdownValue", "wmsOverlays.setStyle", style->mName, false);
      mGuiManager->getGui()->callJavascript(
          "CosmoScout.wmsOverlays.setLegendURL", style->mLegendUrl.value_or(""));
    }

    bodySettings.mActiveStyle.set(name);
    wmsOverlay->setStyle(name);
  } else {
    resetWMSStyle(wmsOverlay);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::resetWMSStyle(std::shared_ptr<TextureOverlayRenderer> const& wmsOverlay) {
  if (isActiveOverlay(wmsOverlay)) {
    mGuiManager->getGui()->callJavascript(
        "CosmoScout.gui.setDropdownValue", "wmsOverlays.setStyle", "", false);
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.setLegendURL", "");
  }

  auto& settings = getBodySettings(wmsOverlay);
  settings.mActiveStyle.set("");
  wmsOverlay->setStyle("");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool Plugin::isActiveOverlay(std::shared_ptr<TextureOverlayRenderer> const& wmsOverlay) {
  return mActiveOverlay && wmsOverlay && wmsOverlay->getCenter() == mActiveOverlay->getCenter();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool Plugin::isActiveOverlay(std::string const& center) {
  return mActiveOverlay && center == mActiveOverlay->getCenter();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool Plugin::addLayerToSelect(std::shared_ptr<TextureOverlayRenderer> const& wmsOverlay,
    WebMapLayer const& layer, std::string const& activeLayer, int const& depth) {
  bool active = layer.getName() == activeLayer;

  if (isActiveOverlay(wmsOverlay)) {
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.addLayer", layer.getName(),
        layer.getTitle(), active, layer.isRequestable(), depth);
  }

  bool anyActive = active;
  if (active) {
    setWMSLayer(wmsOverlay, layer.getName());
  }

  for (auto const& sublayer : layer.getAllLayers()) {
    if (addLayerToSelect(wmsOverlay, sublayer, activeLayer, depth + 1)) {
      anyActive = true;
    }
  }
  return anyActive;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::goToBounds(Bounds const& bounds) {
  double lon      = (bounds.mMinLon + bounds.mMaxLon) / 2.;
  double lat      = (bounds.mMinLat + bounds.mMaxLat) / 2.;
  double lonRange = bounds.mMaxLon - bounds.mMinLon;
  double latRange = bounds.mMaxLat - bounds.mMinLat;

  VistaTransformMatrix proj =
      GetVistaSystem()->GetDisplayManager()->GetCurrentRenderInfo()->m_matProjection;
  double fovy = 2.0 * atan(1.0 / proj[1][1]);
  double fovx = 2.0 * atan(1.0 / proj[0][0]);

  // Rough approximation of the height, at which the whole bounds are in frame
  double radius = mSolarSystem->getRadii(mActiveOverlay->getCenter())[0];
  double heighty =
      std::tan(cs::utils::convert::toRadians(latRange) / 2.) * radius / std::tan(fovy / 2.);
  double heightx =
      std::tan(cs::utils::convert::toRadians(lonRange) / 2.) * radius / std::tan(fovx / 2.);
  heightx -= radius * (1 - std::cos(cs::utils::convert::toRadians(lonRange) / 2.));
  heighty -= radius * (1 - std::cos(cs::utils::convert::toRadians(latRange) / 2.));

  mSolarSystem->flyObserverTo(mSolarSystem->pActiveBody.get()->getCenterName(),
      mSolarSystem->pActiveBody.get()->getFrameName(),
      cs::utils::convert::toRadians(glm::dvec2(lon, lat)), std::max(heighty, heightx), 5.);
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void Plugin::checkScale(Bounds const& bounds, WebMapLayer const& layer, int const& maxTextureSize) {
  static constexpr double metersPerDegree = (6378137. * 2. * glm::pi<double>()) / 360.;
  static constexpr double metersPerPixel  = 0.00028;

  double lonRange = bounds.mMaxLon - bounds.mMinLon;
  double latRange = bounds.mMaxLat - bounds.mMinLat;

  double scaleDenominator =
      std::max(lonRange, latRange) * metersPerDegree / maxTextureSize / metersPerPixel;

  mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.setScale", scaleDenominator);
  if (layer.getSettings().mMinScale && scaleDenominator <= layer.getSettings().mMinScale) {
    std::stringstream warning;
    warning << "The current scale is marked as inappropriate for this layer. ";
    warning << "Scale should be at least 1:" << layer.getSettings().mMinScale.value() << ". ";
    warning << "Consider moving the camera further from the planet or lowering the map resolution.";
    mGuiManager->getGui()->callJavascript(
        "CosmoScout.wmsOverlays.showScaleWarning", true, warning.str());
  } else if (layer.getSettings().mMaxScale && scaleDenominator > layer.getSettings().mMaxScale) {
    std::stringstream warning;
    warning << "The current scale is marked as inappropriate for this layer. ";
    warning << "Scale should be at most 1:" << layer.getSettings().mMaxScale.value() << ". ";
    warning << "Consider moving the camera closer to the planet or increasing the map resolution.";
    mGuiManager->getGui()->callJavascript(
        "CosmoScout.wmsOverlays.showScaleWarning", true, warning.str());
  } else {
    mGuiManager->getGui()->callJavascript("CosmoScout.wmsOverlays.showScaleWarning", false);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace csp::wmsoverlays
