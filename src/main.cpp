#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <array>

using namespace geode::prelude;

static constexpr float kScale = 0.6f;
static constexpr float kPad = 10.f;

static int g_frame = 0;
static int g_lastClick = -1;
static std::array<int, 7> g_counts = {0, 0, 0, 0, 0, 0, 0};

static const char* g_names[7] = {"9+:", "7-8:", "5-6:", "4:", "3:", "2:", "1:"};
static const ccColor3B g_colors[7] = {
    {140, 120, 255}, {110, 190, 255}, {110, 230, 130}, {255, 255, 255},
    {240, 240, 90}, {255, 170, 80}, {240, 80, 90}
};

static int rowForWidth(int w) {
    if (w >= 9) return 0;
    if (w >= 7) return 1;
    if (w >= 5) return 2;
    if (w == 4) return 3;
    if (w == 3) return 4;
    if (w == 2) return 5;
    return 6;
}

class $modify(FrameBase, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (PlayLayer::get() && !isHalfTick) g_frame++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (PlayLayer::get() && isPlayer1 && button == 1 && down) {
            if (g_lastClick >= 0) {
                // TEMPORAL: usa el hueco entre clics, NO la ventana real.
                int gap = g_frame - g_lastClick;
                g_counts[rowForWidth(gap)]++;
            }
            g_lastClick = g_frame;
        }
    }
};

class $modify(FrameLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* frameLabel = nullptr;
        std::array<CCLabelBMFont*, 7> values = {};
        CheckpointObject* saved = nullptr;
        int savedFrame = 0;
    };

    void resetCounters() {
        g_frame = 0;
        g_lastClick = -1;
        g_counts.fill(0);
    }

    void onSave(CCObject*) {
        if (!m_isPracticeMode) return;
        if (m_fields->saved) m_fields->saved->release();
        m_fields->saved = this->createCheckpoint();
        if (m_fields->saved) m_fields->saved->retain();
        m_fields->savedFrame = g_frame;
    }

    void onRestore(CCObject*) {
        if (!m_fields->saved) return;
        this->loadFromCheckpoint(m_fields->saved);
        g_frame = m_fields->savedFrame;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        resetCounters();

        CCNode* parent = this;
        if (m_uiLayer) parent = m_uiLayer;
        auto winSize = CCDirector::get()->getWinSize();

        // Medidas para alinear las columnas
        auto probe1 = CCLabelBMFont::create("7-8:", "bigFont.fnt");
        probe1->setScale(kScale);
        float labelW = probe1->getScaledContentSize().width;
        float rowH = probe1->getScaledContentSize().height * 1.25f;

        auto probe2 = CCLabelBMFont::create("0000", "bigFont.fnt");
        probe2->setScale(kScale);
        float valueW = probe2->getScaledContentSize().width;

        float gap = 12.f;
        float totalW = labelW + gap + valueW + kPad * 2.f;
        float totalH = rowH * 7.f + kPad * 2.f;
        float left = 4.f;
        float top = winSize.height - 4.f;

        // Fondo oscuro
        auto bg = CCLayerColor::create(ccColor4B{0, 0, 0, 110}, totalW, totalH);
        bg->setPosition({left, top - totalH});
        bg->setZOrder(99);
        parent->addChild(bg);

        // Filas: etiqueta + valor
        for (int i = 0; i < 7; i++) {
            float y = top - kPad - i * rowH;

            auto name = CCLabelBMFont::create(g_names[i], "bigFont.fnt");
            name->setScale(kScale);
            name->setAnchorPoint({0.f, 1.f});
            name->setPosition({left + kPad, y});
            name->setColor(g_colors[i]);
            name->setZOrder(100);
            parent->addChild(name);

            auto value = CCLabelBMFont::create("0", "bigFont.fnt");
            value->setScale(kScale);
            value->setAnchorPoint({0.f, 1.f});
            value->setPosition({left + kPad + labelW + gap, y});
            value->setColor(g_colors[i]);
            value->setZOrder(100);
            parent->addChild(value);
            m_fields->values[i] = value;
        }

        // Contador de frame debajo de la tabla
        auto fl = CCLabelBMFont::create("Frame: 0", "bigFont.fnt");
        fl->setScale(kScale * 0.6f);
        fl->setAnchorPoint({0.f, 1.f});
        fl->setPosition({left + kPad, top - totalH - 4.f});
        fl->setZOrder(100);
        parent->addChild(fl);
        m_fields->frameLabel = fl;

        // Botones Guardar / Restaurar (solo funcionan en modo práctica)
        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setZOrder(100);
        auto saveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Guardar"), this, menu_selector(FrameLayer::onSave));
        auto restoreBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Restaurar"), this, menu_selector(FrameLayer::onRestore));
        saveBtn->setPosition({winSize.width - 70.f, winSize.height - 40.f});
        restoreBtn->setPosition({winSize.width - 70.f, winSize.height - 80.f});
        menu->addChild(saveBtn);
        menu->addChild(restoreBtn);
        parent->addChild(menu);

        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (m_fields->frameLabel) {
            std::string s = fmt::format("Frame: {}", g_frame);
            m_fields->frameLabel->setString(s.data());
        }
        for (int i = 0; i < 7; i++) {
            if (m_fields->values[i]) {
                std::string s = fmt::format("{}", g_counts[i]);
                m_fields->values[i]->setString(s.data());
            }
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        resetCounters();
    }
};
