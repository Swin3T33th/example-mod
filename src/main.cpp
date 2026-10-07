#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <vector>

using namespace geode::prelude;

// ---------- Ajustes ----------
static constexpr float kScale = 0.6f;
static constexpr float kPad = 10.f;

static constexpr int kBack = 5;
static constexpr int kFwd = 5;
static constexpr int kSnapEvery = 5;
static constexpr int kMargin = 2;
static constexpr int kHorizon = 120;      // frames que se analizan tras el click
static constexpr size_t kMaxPending = 30;
static constexpr int kNoClick = 1000;
static constexpr int kNever = 0x7fffffff;
static constexpr float kPosTol = 0.1f;
static constexpr float kStepDt = 0.9999f / 240.f;
static constexpr bool kStrictDrift = true;
static constexpr bool kAnyTrigger = false;
static constexpr int kTrigLookback = 480;
static constexpr int kTrigKeep = 1500;
static constexpr bool kDebug = true;
static constexpr bool kDisableIfXdbot = false;
static constexpr bool kUsePlayLayerUpdate = false;

// Convergencia: frames seguidos con la misma posicion que la real para dar el sondeo por superado
static constexpr int kConvFrames = 3;
static constexpr float kConvTol = 0.01f;

// Desfase de la carga del checkpoint: frames maximos que se buscan hacia atras
static constexpr int kLagMax = 8;

// Al morir: los clicks mas cerca que esto de la muerte se descartan (probable click fallido)
static constexpr int kDeathMargin = 2;
static constexpr int kMinSpan = 30;
// Historial de posiciones reales (frames)
static constexpr size_t kPosKeep = 2400;

static constexpr float kBudgetMs = 10.f;
static constexpr float kBudgetSlowMs = 3.f;
static constexpr float kSlowFrameMs = 20.f;

// ---------- Estado ----------
// El contador de frames NO se reinicia al reaparecer (solo al entrar al nivel)
static int g_frame = 0;
static int g_deathFrame = -1;
static std::array<int, 7> g_counts = {0, 0, 0, 0, 0, 0, 0};
// 0 son, 1 ok, 2 sc, 3 d0, 4 pas, 5 clk, 6 inv, 7 dev, 8 trg, 9 perd, 10 resc, 11 cola
static std::array<int, 12> g_dbg = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static int g_lastAdv = 1;
static bool g_hudDirty = true;

static int g_diagStep = -1;
static float g_diagDx = 0.f;
static float g_diagDy = 0.f;
static float g_diagEffect = -1.f;
static float g_diagXsim = 0.f;
static float g_diagXreal = 0.f;
static float g_endY = 0.f;
static float g_tw = 1.f;
static float g_restDx = -1.f;
static float g_restDy = -1.f;
static long long g_stepsTotal = 0;
static long long g_runsTotal = 0;
static int g_convCount = 0;

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

static bool affectsGameplay(int id) {
    if (kAnyTrigger) return true;
    switch (id) {
        case 901:
        case 1346:
        case 2067:
        case 1347:
        case 1814:
        case 1049:
        case 1268:
        case 1616:
        case 1912:
        case 2068:
            return true;
        default:
            return false;
    }
}

struct Input { int frame; bool down; };
struct Snap { int frame; CheckpointObject* cp; };
struct Open { CheckpointObject* base = nullptr; int baseFrame = 0; int clickFrame = -1; };
struct Pos { int frame; float x; float y; };

struct Pending {
    CheckpointObject* base = nullptr;
    int baseFrame = 0;
    int clickFrame = 0;
    int endFrame = 0;
    int upFrame = kNever;        // frame real en que se suelta ese click (kNever = no se solto a tiempo)
    int lag = -1;                // desfase propio de esta ventana (-1 = sin medir)
    bool heldAtStart = false;    // boton pulsado en baseFrame - kLagMax
    std::vector<Input> inputs;   // desde baseFrame - kLagMax
    int dMin = 0;
    int dMax = 0;
    // 0 clic real, 1 sin clic, 2 barrido hacia atras, 3 hacia delante, 4 barrido completo
    int stage = 0;
    int d = 0;
    int w = -1;                  // ventana final; -1 = sin medir
    float y0 = 0.f;
    // resultado por desplazamiento d (indice d+8): -1 sin probar, 0 muere, 1 sobrevive
    std::array<signed char, 16> res{};
};

static std::deque<Snap> g_ring;
static std::deque<Pos> g_posLog;
static std::deque<Input> g_log;
static std::deque<int> g_trigFrames;
static std::vector<Open> g_waiting;
static std::deque<Pending> g_pending;
static bool g_probing = false;
static bool g_inPost = false;
static bool g_own = false;
static bool g_probeDead = false;
static bool g_probeInvalid = false;
static int g_probeTick = 0;
static std::vector<Pos> g_simLog;
static int g_probeBase = 0;
static int g_probeTick0 = 0;
static float g_diagMovReal = 0.f;
static float g_diagMovSim = 0.f;
static float g_restX = 0.f;
static float g_sim0X = 0.f;
static float g_real0X = 0.f;
// ultimo desfase medido (diagnostico y valor de reserva)
static int g_lag = -1;
static int g_lagMin = 99;
static int g_lagMax = -1;
static float g_lagErr = -1.f;

// Limpieza total (entrar/salir del nivel)
static void clearProbeState() {
    for (auto& s : g_ring) s.cp->release();
    g_ring.clear();
    for (auto& o : g_waiting) o.base->release();
    g_waiting.clear();
    for (auto& p : g_pending) p.base->release();
    g_pending.clear();
    g_log.clear();
    g_posLog.clear();
    g_trigFrames.clear();
    g_simLog.clear();
    g_probing = false;
    g_inPost = false;
    g_own = false;
    g_deathFrame = -1;
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

// Posicion real registrada ANTES de ejecutar el frame "frame"
static const Pos* posAt(int frame) {
    if (g_posLog.empty()) return nullptr;
    int idx = frame - g_posLog.front().frame;
    if (idx < 0 || idx >= static_cast<int>(g_posLog.size())) return nullptr;
    const Pos* p = &g_posLog[idx];
    return p->frame == frame ? p : nullptr;
}

static size_t logLowerBound(int frame) {
    size_t lo = 0, hi = g_log.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_log[mid].frame < frame) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Estado del boton justo antes de ejecutar el frame "frame"
static bool heldAtFrame(int frame) {
    size_t lb = logLowerBound(frame);
    return lb > 0 ? g_log[lb - 1].down : false;
}

static bool windowHasTrigger(int a, int b) {
    for (auto it = g_trigFrames.rbegin(); it != g_trigFrames.rend(); ++it) {
        if (*it < a) break;
        if (*it < b) return true;
    }
    return false;
}

static std::vector<Input> shiftedInputs(const Pending& p, int d) {
    std::vector<Input> out = p.inputs;
    if (d == 0) return out;
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

// Racha mas larga de desplazamientos que sobreviven (empate: la mas cercana a 0)
static int bestRun(const Pending& p) {
    int best = 0;
    int bestDist = 1000;
    int d = p.dMin;
    while (d <= p.dMax) {
        if (p.res[d + 8] == 1) {
            int s = d;
            while (d <= p.dMax && p.res[d + 8] == 1) d++;
            int len = d - s;
            int dist = (s <= 0 && d > 0) ? 0 : std::min(std::abs(s), std::abs(d - 1));
            if (len > best || (len == best && dist < bestDist)) {
                best = len;
                bestDist = dist;
            }
        } else {
            d++;
        }
    }
    return best;
}

// Convierte un click en espera en un trabajo de sondeo con la ventana [base, end)
static void queueJob(const Open& o, int end) {
    if (windowHasTrigger(o.baseFrame - kTrigLookback, end)) {
        g_dbg[8]++;
        g_hudDirty = true;
        o.base->release();
        return;
    }

    Pending p;
    p.base = o.base;
    p.baseFrame = o.baseFrame;
    p.clickFrame = o.clickFrame;
    p.endFrame = end;
    p.res.fill(-1);

    // frame real en que se suelta este click
    for (size_t j = logLowerBound(o.clickFrame + 1); j < g_log.size(); j++) {
        if (!g_log[j].down) {
            if (g_log[j].frame < end) p.upFrame = g_log[j].frame;
            break;
        }
    }

    // los inputs se recogen desde kLagMax frames antes de la base,
    // porque el sondeo puede empezar hasta kLagMax frames antes
    p.heldAtStart = heldAtFrame(o.baseFrame - kLagMax);
    size_t lb = logLowerBound(o.baseFrame - kLagMax);
    for (size_t j = lb; j < g_log.size() && g_log[j].frame < end; j++) {
        p.inputs.push_back(g_log[j]);
    }
    p.dMin = std::max(-kBack, o.baseFrame - o.clickFrame);
    p.dMax = std::max(0, std::min(kFwd, end - kMargin - 1 - o.clickFrame));
    g_pending.push_back(std::move(p));
}

// Al reaparecer: rescata los clicks que esperaban, recortando su ventana a la muerte.
// La cola de trabajos y el historial se conservan para terminar el analisis.
static void rescueWaiting() {
    int cap = (g_deathFrame >= 0) ? g_deathFrame - kDeathMargin : g_frame - 1;
    for (auto& o : g_waiting) {
        int end = std::min(o.clickFrame + kHorizon, cap);
        if (end - o.clickFrame < kMinSpan) {
            o.base->release();
            g_dbg[9]++;
            continue;
        }
        g_dbg[10]++;
        queueJob(o, end);
    }
    g_waiting.clear();

    // las instantaneas del intento anterior no valen para clicks nuevos
    for (auto& s : g_ring) s.cp->release();
    g_ring.clear();

    g_deathFrame = -1;
    g_probing = false;
    g_inPost = false;
    g_own = false;
    g_hudDirty = true;
}

class $modify(FrameTrigger, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int p1, gd::vector<int> const* p2) {
        if (g_probing) return;
        if (PlayLayer::get() && affectsGameplay(m_objectID)) {
            g_trigFrames.push_back(g_frame);
            while (!g_trigFrames.empty() && g_trigFrames.front() < g_frame - kTrigKeep) {
                g_trigFrames.pop_front();
            }
        }
        EffectGameObject::triggerObject(layer, p1, p2);
    }
};

class $modify(FrameBase, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        if (pl && !isHalfTick && !g_probing && m_player1) {
            g_posLog.push_back({g_frame, m_player1->getPositionX(), m_player1->getPositionY()});
            while (g_posLog.size() > kPosKeep) g_posLog.pop_front();
        }
        if (pl && pl->m_isPracticeMode && pl->m_player1 && !isHalfTick && !g_probing) {
            if (g_frame % kSnapEvery == 0) pushSnap(pl);
        }
        if (pl && !isHalfTick && g_probing && m_player1) {
            g_simLog.push_back({g_probeBase + (g_probeTick - g_probeTick0),
                m_player1->getPositionX(), m_player1->getPositionY()});
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (pl && !isHalfTick) {
            if (g_probing) g_probeTick++;
            else g_frame++;
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        // Ignora clics de otros mods (xdBot) mientras se sondea
        if (g_probing && !g_own) return;

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (g_probing) return;
        if (!PlayLayer::get() || !isPlayer1 || button != 1) return;

        g_log.push_back({g_frame, down});
        if (g_log.size() > 400) g_log.pop_front();
        if (!down) return;
        g_dbg[5]++;
        g_hudDirty = true;

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
        CCLabelBMFont* diagLabel = nullptr;
        std::array<CCLabelBMFont*, 7> values = {};
        int hudTick = 0;
    };

    // Entrada a un nivel: reinicio completo
    void resetAll() {
        g_frame = 0;
        g_lag = -1;
        clearProbeState();
    }

    bool stepFrame() {
        // Si tu version de Geode no tiene m_timeWarp, usa: float dt = kStepDt;
        g_tw = m_gameState.m_timeWarp;
        float dt = kStepDt / std::max(0.01f, m_gameState.m_timeWarp);
        int advanced = 0;
        for (int attempt = 0; attempt < 2 && advanced == 0; attempt++) {
            int before = g_probeTick;
            if (kUsePlayLayerUpdate) PlayLayer::update(dt);
            else GJBaseGameLayer::update(dt);
            advanced = g_probeTick - before;
        }
        g_lastAdv = advanced;
        if (advanced != 1) {
            log::warn("Sondeo: update({}) avanzo {} frames (se esperaba 1)", dt, advanced);
            g_dbg[6]++;
            g_probeInvalid = true;
            return false;
        }
        return true;
    }

    // Mide cuantos frames "retrocede" la carga del checkpoint de ESTA ventana:
    // da un paso, mira la x al empezar ese paso y busca a que frame real corresponde.
    bool calibrateLag(Pending& p) {
        this->loadFromCheckpoint(p.base);
        if (!m_player1) return false;

        g_probing = true;
        g_probeDead = false;
        g_probeInvalid = false;
        g_simLog.clear();
        g_probeBase = p.baseFrame;
        g_probeTick0 = g_probeTick;

        bool ok = stepFrame();
        g_probing = false;
        g_own = false;
        if (!ok || g_simLog.empty()) {
            g_simLog.clear();
            return false;
        }

        float sx = g_simLog[0].x;
        g_simLog.clear();

        int best = -1;
        float bestErr = 1e9f;
        for (int k = 0; k <= kLagMax; k++) {
            auto rp = posAt(p.baseFrame - k);
            if (!rp) continue;
            float e = std::fabs(sx - rp->x);
            if (e < bestErr) { bestErr = e; best = k; }
        }
        g_lagErr = bestErr;
        g_hudDirty = true;

        if (best >= 0 && bestErr <= kPosTol) {
            p.lag = best;
            g_lag = best;
            g_lagMin = std::min(g_lagMin, best);
            g_lagMax = std::max(g_lagMax, best);
            return true;
        }
        // sin dato fiable: reutiliza el ultimo desfase conocido, o descarta
        if (g_lag >= 0) {
            p.lag = g_lag;
            return true;
        }
        return false;
    }

    // Carga el checkpoint y deja todo listo para sondear.
    // t0 = frame real desde el que empieza el bucle (baseFrame - desfase).
    bool startProbe(Pending& p, const std::vector<Input>& inputs, int& t0) {
        if (p.lag < 0 && !calibrateLag(p)) return false;
        t0 = p.baseFrame - p.lag;

        this->loadFromCheckpoint(p.base);
        if (!m_player1) return false;

        g_probing = true;
        g_probeDead = false;
        g_probeInvalid = false;
        g_simLog.clear();
        g_probeBase = t0;
        g_probeTick0 = g_probeTick;

        // boton mantenido al empezar, segun los inputs de esta simulacion
        bool held = p.heldAtStart;
        for (auto& in : inputs) {
            if (in.frame >= t0) break;
            held = in.down;
        }
        if (held) {
            g_own = true;
            this->handleButton(true, 1, true);
            g_own = false;
        }
        return true;
    }

    // 1 sobrevive, 0 muere, -1 invalido, -2 desviado (solo con kStrictDrift)
    int survives(Pending& p, int d) {
        auto inputs = shiftedInputs(p, d);

        // Desde este frame, los inputs de la simulacion son identicos a los reales
        int lastDiff = kNever;
        if (p.upFrame != kNever) {
            lastDiff = (d == kNoClick) ? p.upFrame : std::max(p.upFrame, p.upFrame + d);
        }

        int t0 = p.baseFrame;
        if (!startProbe(p, inputs, t0)) {
            g_probing = false;
            g_own = false;
            return -1;
        }

        if (kDebug && d == 0 && m_player1) {
            if (auto bp = posAt(p.baseFrame)) {
                g_restDx = std::fabs(m_player1->getPositionX() - bp->x);
                g_restDy = std::fabs(m_player1->getPositionY() - bp->y);
                g_restX = m_player1->getPositionX();
            }
        }
        if (kDebug && d == 0) g_diagStep = -1;

        size_t k = 0;
        while (k < inputs.size() && inputs[k].frame < t0) k++;   // inputs anteriores al inicio

        int conv = 0;
        bool converged = false;
        int steps = 0;

        for (int t = t0; t < p.endFrame && !g_probeDead && !g_probeInvalid; t++) {
            while (k < inputs.size() && inputs[k].frame <= t) {
                g_own = true;
                this->handleButton(inputs[k].down, 1, true);
                g_own = false;
                k++;
            }
            if (!stepFrame()) break;
            steps++;
            if (m_player1 && m_player1->m_isDead) g_probeDead = true;
            if (g_probeDead) break;

            // Salida temprana: ya no quedan diferencias de input y la simulacion
            // iguala la trayectoria real durante kConvFrames frames seguidos
            if (t >= lastDiff && m_player1) {
                auto rp = posAt(t + 1);
                if (rp &&
                    std::fabs(m_player1->getPositionX() - rp->x) <= kConvTol &&
                    std::fabs(m_player1->getPositionY() - rp->y) <= kConvTol) {
                    if (++conv >= kConvFrames) { converged = true; break; }
                } else {
                    conv = 0;
                }
            }
        }
        g_probing = false;
        g_own = false;
        g_stepsTotal += steps;
        g_runsTotal++;
        if (converged) g_convCount++;
        if (g_probeInvalid) return -1;
        if (!m_player1) return -1;

        g_endY = m_player1->getPositionY();

        bool bad = false;
        if (d == 0) {
            for (size_t i = 0; i < g_simLog.size(); i++) {
                auto rp = posAt(g_simLog[i].frame);
                if (!rp) continue;
                float dx = std::fabs(g_simLog[i].x - rp->x);
                float dy = std::fabs(g_simLog[i].y - rp->y);
                if (dx > kPosTol || dy > kPosTol) {
                    g_diagStep = static_cast<int>(i);
                    g_diagDx = dx;
                    g_diagDy = dy;
                    bad = true;
                    break;
                }
            }
            if (!g_simLog.empty()) {
                g_sim0X = g_simLog[0].x;
                if (auto r = posAt(g_simLog[0].frame)) g_real0X = r->x;
            }
            if (kDebug && g_simLog.size() > 1) {
                auto r0 = posAt(g_simLog[0].frame);
                auto r1 = posAt(g_simLog[0].frame + 1);
                if (r0 && r1) g_diagMovReal = r1->x - r0->x;
                g_diagMovSim = g_simLog[1].x - g_simLog[0].x;
                g_diagXsim = g_simLog.back().x;
                auto re = posAt(g_simLog.back().frame);
                if (re) g_diagXreal = re->x;
            }
            if (bad) g_dbg[7]++;
        }

        if (bad && kStrictDrift) return -2;
        if (g_probeDead) return 0;
        return 1;
    }

    bool finishJob(Pending& p) {
        int run = bestRun(p);
        if (run <= 0) {
            g_dbg[3]++;       // ningun frame sobrevive
        } else {
            p.w = run;
            g_dbg[1]++;
        }
        return true;
    }

    // Avanza un paso del sondeo de un clic. Devuelve true cuando ya esta resuelto.
    bool advanceJob(Pending& p) {
        g_hudDirty = true;
        switch (p.stage) {
            case 0: {
                // Clic real: sirve de referencia, pero NO se descarta si muere
                g_dbg[0]++;
                int r = survives(p, 0);
                if (r == -1) { g_dbg[4]++; return true; }
                if (r == -2) { return true; }
                p.res[8] = (r == 1) ? 1 : 0;
                if (r == 1) p.y0 = g_endY;
                p.stage = 1;
                return false;
            }
            case 1: {
                // Sin clic: si sobrevives igual, el clic no tiene efecto
                int r = survives(p, kNoClick);
                if (r == -1) { g_dbg[4]++; return true; }
                if (r == 1) {
                    g_dbg[2]++;
                    if (kDebug) g_diagEffect = std::fabs(g_endY - p.y0);
                    return true;
                }
                if (p.res[8] == 1) { p.d = -1; p.stage = 2; }       // barrido hacia fuera
                else { p.d = p.dMin; p.stage = 4; }                 // d=0 murio: barrido completo
                return false;
            }
            case 2: {
                // Hacia atras desde d = -1, parando en el primer fallo
                if (p.d < p.dMin) { p.d = 1; p.stage = 3; return false; }
                int r = survives(p, p.d);
                if (r == -1) { g_dbg[4]++; return true; }
                p.res[p.d + 8] = (r == 1) ? 1 : 0;
                if (r == 1) p.d--;
                else { p.d = 1; p.stage = 3; }
                return false;
            }
            case 3: {
                // Hacia delante desde d = 1, parando en el primer fallo
                if (p.d > p.dMax) return finishJob(p);
                int r = survives(p, p.d);
                if (r == -1) { g_dbg[4]++; return true; }
                p.res[p.d + 8] = (r == 1) ? 1 : 0;
                if (r == 1) { p.d++; return false; }
                return finishJob(p);
            }
            default: {
                // Barrido completo (solo si d=0 no sobrevivio)
                if (p.d == 0) p.d++;
                if (p.d > p.dMax) return finishJob(p);
                int r = survives(p, p.d);
                if (r == -1) { g_dbg[4]++; return true; }
                p.res[p.d + 8] = (r == 1) ? 1 : 0;
                p.d++;
                return false;
            }
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_probing) {
            g_probeDead = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
        // solo cuenta como muerte si de verdad murio (otro mod puede evitarla)
        if (g_deathFrame < 0 && m_player1 && m_player1->m_isDead) {
            g_deathFrame = g_frame;
        }
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        resetAll();
        g_counts.fill(0);
        g_dbg.fill(0);
        g_diagStep = -1;
        g_diagEffect = -1.f;
        g_restDx = -1.f;
        g_restDy = -1.f;
        g_diagDx = 0.f;
        g_diagDy = 0.f;
        g_diagXsim = 0.f;
        g_diagXreal = 0.f;
        g_diagMovReal = 0.f;
        g_diagMovSim = 0.f;
        g_restX = 0.f;
        g_sim0X = 0.f;
        g_real0X = 0.f;
        g_lagErr = -1.f;
        g_lagMin = 99;
        g_lagMax = -1;
        g_stepsTotal = 0;
        g_runsTotal = 0;
        g_convCount = 0;
        g_hudDirty = true;

        CCNode* parent = this;
        if (m_uiLayer) parent = m_uiLayer;
        auto winSize = CCDirector::get()->getWinSize();

        auto addLabel = [&](const char* text, float scale, CCPoint pos, ccColor3B color, int z) {
            auto l = CCLabelBMFont::create(text, "bigFont.fnt");
            l->setScale(scale);
            l->setAnchorPoint({0.f, 1.f});
            l->setPosition(pos);
            l->setColor(color);
            l->setZOrder(z);
            parent->addChild(l);
            return l;
        };

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
            addLabel(g_names[i], kScale, ccp(left + kPad, y), g_colors[i], 100);
            m_fields->values[i] = addLabel("0", kScale, ccp(left + kPad + labelW + gap, y), g_colors[i], 100);
        }

        m_fields->frameLabel = addLabel("Frame: 0", kScale * 0.6f,
            ccp(left + kPad, top - totalH - 4.f), ccColor3B{255, 255, 255}, 100);

        if (kDebug) {
            m_fields->dbgLabel = addLabel("-", kScale * 0.5f,
                ccp(left + totalW + 8.f, top - 24.f), ccColor3B{255, 255, 0}, 101);
            m_fields->diagLabel = addLabel("-", kScale * 0.5f,
                ccp(left + totalW + 8.f, top - 100.f), ccColor3B{120, 255, 120}, 101);
        }

        return true;
    }

    void promoteWaiting() {
        for (size_t i = 0; i < g_waiting.size();) {
            Open o = g_waiting[i];
            if (g_frame < o.clickFrame + kHorizon) { i++; continue; }
            g_waiting.erase(g_waiting.begin() + i);
            queueJob(o, o.clickFrame + kHorizon);   // horizonte fijo
        }
    }

    void updateHud() {
        auto& f = m_fields;
        f->hudTick++;
        if (f->frameLabel && f->hudTick % 3 == 0) {
            std::string s = fmt::format("Frame: {} v13", g_frame);
            f->frameLabel->setString(s.c_str());
        }
        if (!g_hudDirty || f->hudTick % 6 != 0) return;
        g_hudDirty = false;

        for (int i = 0; i < 7; i++) {
            if (f->values[i]) {
                std::string s = fmt::format("{}", g_counts[i]);
                f->values[i]->setString(s.c_str());
            }
        }
        if (kDebug && f->dbgLabel) {
            int desc = g_dbg[2] + g_dbg[3] + g_dbg[4] + (kStrictDrift ? g_dbg[7] : 0);
            int curso = g_dbg[0] - g_dbg[1] - desc;
            std::string s = fmt::format(
                "clk {} son {} ok {}\nsc {} d0 {} pas {} dev {} inv {} trg {}\nson {} = ok {} + desc {} + curso {}\nresc {} perd {} cola {}",
                g_dbg[5], g_dbg[0], g_dbg[1], g_dbg[2], g_dbg[3], g_dbg[4], g_dbg[7], g_dbg[6], g_dbg[8],
                g_dbg[0], g_dbg[1], desc, curso,
                g_dbg[10], g_dbg[9], g_dbg[11]);
            f->dbgLabel->setString(s.c_str());
        }
        if (kDebug && f->diagLabel) {
            float avg = g_runsTotal > 0 ? static_cast<float>(g_stepsTotal) / static_cast<float>(g_runsTotal) : 0.f;
            std::string s = fmt::format(
                "dev paso {} dx {:.2f} dy {:.2f}\nefecto click dy {:.2f}\nx sim {:.1f} real {:.1f}\nmov real {:.3f} sim {:.3f}\nadv {} tw {:.3f}\nrest dx {:.2f} dy {:.2f}\nrest x {:.1f} sim0 {:.1f} real0 {:.1f}\nlag {} min {} max {} err {:.3f}\navg pasos {:.1f} conv {}/{}",
                g_diagStep, g_diagDx, g_diagDy, g_diagEffect, g_diagXsim, g_diagXreal,
                g_diagMovReal, g_diagMovSim, g_lastAdv, g_tw, g_restDx, g_restDy,
                g_restX, g_sim0X, g_real0X,
                g_lag, g_lagMin == 99 ? -1 : g_lagMin, g_lagMax, g_lagErr,
                avg, g_convCount, static_cast<int>(g_runsTotal));
            f->diagLabel->setString(s.c_str());
        }
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (g_probing || g_inPost) return;
        g_inPost = true;

        bool skip = kDisableIfXdbot && Loader::get()->isModLoaded("zilko.xdbot");

        if (!skip) promoteWaiting();

        if (!skip && !g_pending.empty() && m_isPracticeMode && m_player1 && !m_player1->m_isDead) {
            while (g_pending.size() > kMaxPending) {
                g_pending.back().base->release();
                g_pending.pop_back();
                g_dbg[11]++;
            }

            auto live = this->createCheckpoint();
            if (live) {
                live->retain();
                float budgetMs = (dt * 1000.f > kSlowFrameMs) ? kBudgetSlowMs : kBudgetMs;
                auto t0 = std::chrono::steady_clock::now();

                while (!g_pending.empty()) {
                    Pending& p = g_pending.front();
                    if (advanceJob(p)) {
                        if (p.w > 0) g_counts[rowForWidth(p.w)]++;
                        p.base->release();
                        g_pending.pop_front();
                    }
                    float ms = std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
                    if (ms >= budgetMs) break;
                }

                this->loadFromCheckpoint(live);
                live->release();
            }
        }

        updateHud();
        g_inPost = false;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        rescueWaiting();   // NO se borra la cola ni el historial
    }

    void onQuit() {
        clearProbeState();
        PlayLayer::onQuit();
    }
};
