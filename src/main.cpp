#include "live_bridge.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/CCDirector.hpp>

using namespace geode::prelude;

class $modify(RhythmLinkDirector, CCDirector) {
    void drawScene() {
        bridge::LiveBridge::get().pumpMainThread();
        CCDirector::drawScene();
    }
};

$on_mod(Loaded) {
    bridge::LiveBridge::get().start();
}
