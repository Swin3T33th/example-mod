#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <vector>

using namespace geode::prelude;

static constexpr float kScale = 0.6f;
static constexpr float kPad = 10.f;

static constexpr int kBack = 5;
static constexpr int kFwd = 5;
static constexpr int kSnapEvery = 4;
static constexpr int kMargin = 2;
static constexpr int kHorizon = 30;
static constexpr int kMinHorizon = 8;
static constexpr int kMaxPending = 8;
static constexpr int kNoClick = 1000;
static constexpr float kPosTol = 0.1f;
static constexpr float kStepDt = 1.001f / 240.f;
// false: si la simulación se desvía de la realidad, se registra pero el click se cuenta igual
static constexpr bool kStrictDrift = false;

static int g_frame = 0;
static std::array<int, 7> g_counts = {0, 0, 0, 0, 0, 0, 0};
static std::array<int, 8> g_dbg = {0, 0, 0, 0, 0, 0, 0, 0};
static int g_lastAdv = 1;

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

struct Input { int frame; bool down; };
struct Snap { int frame; CheckpointObject* cp; };
struct Open { CheckpointObject* base = nullptr; int baseFrame = 0; int clickFrame = -1; };
struct Pending { CheckpointObject* base; int baseFrame; int clickFrame; int endFrame; };
struct Pos { int frame; float x; float y; };

static std::deque<Snap> g_ring;
static std::deque<Pos> g_posLog;
static std::vector<Input> g_log;
static std::vector<Open> g_waiting;
static std::vector<Pending> g_pending;
static bool g_probing = false;
static bool g_probeDead = false;
static bool g_probeInvalid = false;
static int g_probeTick = 0;

static void clearProbeState() {
    for (auto& s : g_ring) s.cp->release();
    g_ring.clear();
    for (auto& o : g_waiting) o.base->release();
    g_waiting.clear();
    for (auto& p : g_pending) p.base->release();
    g_pending.clear();
    g_log.clear();
    g_posLog.clear();
}

static void pushSnap(PlayLayer* pl) {
    auto cp = pl->createCheckpoint();
    if (!cp) return;
    cp->retain();
    g_ring.push_back({g_frame, cp});
    while (g_ring.size() > static_cast<size_t>(kBack / kSnapEvery + 2)) {
        g_ring.front().cp->release();
        g_ring.pop_front();
    }
}

static std::vector<Input> buildInputs(const Pending& p, int d) {
    std::vector<Input> out;
    for (auto& in : g_log) {
        if (in.frame >= p.baseFrame && in.frame < p.endFrame) out.push_back(in);
    }
    int dn = -1, up = -1;
    for (int i = 0; i < static_cast<int>(out.size()); i++) {
        if (dn < 0 && out[i].down && out[i].frame == p.clickFrame) dn = i;
        else if (dn >= 0 && up < 0 && !out[i].down) up = i;
    }
    if (d == kNoClick) {
        if (up >= 0) out.erase(out.begin() + up);
        if (dn >= 0) out.erase(out.begin() + dn);
    } else {
        if (dn >= 0) out[dn].frame += d;
        if (up >= 0) out[up].frame += d;
    }
    std::stable_sort(out.begin(), out.end(),
        [](const Input& a, const Input& b) { return a.frame < b.frame; });
    return out;
}

class $modify(FrameBase, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (pl && !isHalfTick && !g_probing) {
            if (g_frame % kSnapEvery == 0) pushSnap(pl);
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (pl && !isHalfTick) {
            if (g_probing) {
                g_probeTick++;
            } else {
                g_frame++;
                if (m_player1) {
                    g_posLog.push_back({g_frame, m_player1->getPositionX(), m_player1->getPositionY()});
                    while (g_posLog.size() > 600) g_posLog.pop_front();
                }
            }
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (g_probing) return;
        if (!PlayLayer::get() || !isPlayer1 || button != 1) return;

        g_log.push_back({g_frame, down});
        if (!down) return;
        g_dbg[5]++;

        Snap* best = nullptr;
        for (auto& s : g_ring) {
            if (s.frame <= g_frame - kBack) best = &s;
        }
        if (!best && !g_ring.empty()) best = &g_ring.front();
        if (!best) return;

        best->cp->retain();
        g_waiting.push_back(Open{best->cp, best->frame, g_frame});
        if (g_waiting.size() > 16) {
            g_waiting.front().base->release();
            g_waiting.erase(g_waiting.begin());
        }
    }
};

class $modify(FrameLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* frameLabel = nullptr;
        CCLabelBMFont* dbgLabel = nullptr;
        std::array<CCLabelBMFont*, 7> values = {};
        CheckpointObject* saved = nullptr;
        int savedFrame = 0;
    };

    void resetFrameState() {
        g_frame = 0;
        clearProbeState();
    }

    bool stepFrame() {
        // Si tu versión de Geode no tiene m_timeWarp, usa: float dt = kStepDt;
        float dt = kStepDt / std::max(0.01f, m_gameState.m_timeWarp);
        int advanced = 0;
        // Reintenta si el resto de tiempo acumulado hizo que no avanzara ningún frame
        for (int attempt = 0; attempt < 2 && advanced == 0; attempt++) {
            int before = g_probeTick;
            GJBaseGameLayer::update(dt);
            advanced = g_probeTick - before;
        }
        g_lastAdv = advanced;
        if (advanced != 1) {
            log::warn("Sondeo: update({}) avanzó {} frames (se esperaba 1)", dt, advanced);
            g_probeInvalid = true;
            return false;
        }
        return true;
    }

    int survives(const Pending& p, int d) {
        this->loadFromCheckpoint(p.base);
        auto inputs = buildInputs(p, d);
        g_probing = true;
        g_probeDead = false;
        g_probeInvalid = false;
        size_t k = 0;
        for (int t = p.baseFrame; t < p.endFrame && !g_probeDead && !g_probeInvalid; t++) {
            while (k < inputs.size() && inputs[k].frame <= t) {
                this->handleButton(inputs[k].down, 1, true);
                k++;
            }
            if (!stepFrame()) break;
            if (m_player1 && m_player1->m_isDead) g_probeDead = true;
        }
        g_probing = false;
        if (g_probeInvalid) return -1;
        if (g_probeDead) return 0;

        if (d == 0) {
            bool found = false;
            for (auto it = g_posLog.rbegin(); it != g_posLog.rend(); ++it) {
                if (it->frame == p.endFrame) {
                    found = true;
                    float dx = std::fabs(m_player1->getPositionX() - it->x);
                    float dy = std::fabs(m_player1->getPositionY() - it->y);
                    if (dx > kPosTol || dy > kPosTol) {
                        g_dbg[7]++;
                        log::info("Click f{}: desviación dx={} dy={}", p.clickFrame, dx, dy);
                        if (kStrictDrift) return -2;
                    }
                    break;
                }
            }
            if (!found) return -1;
        }
        return 1;
    }

    int probeWindow(const Pending& p) {
        auto live = this->createCheckpoint();
        if (!live) return -1;
        live->retain();
        g_dbg[0]++;

        int dMin = std::max(-kBack, p.baseFrame - p.clickFrame);
        int dMax = std::max(0, std::min(kFwd, p.endFrame - kMargin - 1 - p.clickFrame));

        int w = -1;
        int r0 = survives(p, 0);
        if (r0 == -1) {
            g_dbg[4]++;
        } else if (r0 == -2) {
            log::info("Click f{}: la simulación se desvía de la realidad, descartado", p.clickFrame);
        } else if (r0 == 0) {
            g_dbg[3]++;
            log::warn("Sondeo: d=0 murió; click en frame {} descartado", p.clickFrame);
        } else {
            int rNo = survives(p, kNoClick);
            if (rNo == -1) {
                g_dbg[4]++;
            } else if (rNo == 1) {
                g_dbg[2]++;
                log::info("Click f{}: no hace falta (sobrevive sin click), descartado", p.clickFrame);
            } else {
                w = 1;
                for (int d = -1; d >= dMin; d--) {
                    if (survives(p, d) != 1) break;
                    w++;
                }
                for (int d = 1; d <= dMax; d++) {
                    if (survives(p, d) != 1) break;
                    w++;
                }
                g_dbg[1]++;
                log::info("Click f{}: ventana {}", p.clickFrame, w);
            }
        }

        this->loadFromCheckpoint(live);
        live->release();
        return w;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_probing) {
            g_probeDead = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
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
        clearProbeState();
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        resetFrameState();
        g_counts.fill(0);
        g_dbg.fill(0);

        CCNode* parent = this;
        if (m_uiLayer) parent = m_uiLayer;
        auto winSize = CCDirector::get()->getWinSize();

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

        auto bg = CCLayerColor::create(ccColor4B{0, 0, 0, 110}, totalW, totalH);
        bg->setPosition({left, top - totalH});
        bg->setZOrder(99);
        parent->addChild(bg);

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

        auto fl = CCLabelBMFont::create("Frame: 0", "bigFont.fnt");
        fl->setScale(kScale * 0.6f);
        fl->setAnchorPoint({0.f, 1.f});
        fl->setPosition({left + kPad, top - totalH - 4.f});
        fl->setZOrder(100);
        parent->addChild(fl);
        m_fields->frameLabel = fl;

        auto dl = CCLabelBMFont::create("-", "bigFont.fnt");
        dl->setScale(kScale * 0.5f);
        dl->setAnchorPoint({0.f, 1.f});
        dl->setPosition({left + totalW + 8.f, top - 24.f});
        dl->setColor({255, 255, 0});
        dl->setZOrder(101);
        parent->addChild(dl);
        m_fields->dbgLabel = dl;

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setZOrder(100);
        auto saveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Guardar", 80, true, "bigFont.fnt", "GJ_button_01.png", 50.f, 0.35f),
            this, menu_selector(FrameLayer::onSave));
        auto restoreBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Restaurar", 80, true, "bigFont.fnt", "GJ_button_01.png", 50.f, 0.35f),
            this, menu_selector(FrameLayer::onRestore));
        saveBtn->setPosition({winSize.width / 2.f - 48.f, 35.f});
        restoreBtn->setPosition({winSize.width / 2.f + 48.f, 35.f});
        menu->addChild(saveBtn);
        menu->addChild(restoreBtn);
        parent->addChild(menu);

        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        for (size_t i = 0; i < g_waiting.size();) {
            if (g_frame >= g_waiting[i].clickFrame + kHorizon) {
                auto& o = g_waiting[i];
                int end = o.clickFrame + kHorizon;
                for (auto& in : g_log) {
                    if (in.down && in.frame > o.clickFrame) {
                        end = std::min(end, in.frame);
                        break;
                    }
                }
                end = std::max(end, o.clickFrame + kMinHorizon);
                g_pending.push_back({o.base, o.baseFrame, o.clickFrame, end});
                g_waiting.erase(g_waiting.begin() + i);
            } else {
                i++;
            }
        }

        if (!g_pending.empty()) {
            while (g_pending.size() > static_cast<size_t>(kMaxPending)) {
                g_pending.front().base->release();
                g_pending.erase(g_pending.begin());
            }
            Pending p = g_pending.front();
            g_pending.erase(g_pending.begin());
            int w = probeWindow(p);
            if (w > 0) g_counts[rowForWidth(w)]++;
            p.base->release();
        }

        if (m_fields->frameLabel) {
            std::string s = fmt::format("Frame: {}", g_frame);
            m_fields->frameLabel->setString(s.data());
        }
        if (m_fields->dbgLabel) {
            std::string s = fmt::format("clk {} son {} ok {}\nsc {} d0 {} pas {} dev {} adv {}",
                g_dbg[5], g_dbg[0], g_dbg[1], g_dbg[2], g_dbg[3], g_dbg[4], g_dbg[7], g_lastAdv);
            m_fields->dbgLabel->setString(s.data());
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
        resetFrameState();
    }

    void onQuit() {
        clearProbeState();
        PlayLayer::onQuit();
    }
};
