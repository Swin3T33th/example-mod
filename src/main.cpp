#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

static int g_frame = 0;

class $modify(FrameBase, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (PlayLayer::get() && !isHalfTick) g_frame++;
    }
};

class $modify(FrameLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        g_frame = 0;
        auto label = CCLabelBMFont::create("Frame: 0", "bigFont.fnt");
        label->setScale(0.5f);
        label->setAnchorPoint({0.f, 1.f});
        label->setPosition({5.f, CCDirector::get()->getWinSize().height - 5.f});
        label->setZOrder(100);
        this->addChild(label);
        m_fields->label = label;
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (m_fields->label) {
            m_fields->label->setString(fmt::format("Frame: {}", g_frame).c_str());
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_frame = 0;
    }
};
