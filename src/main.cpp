#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <vector>

using namespace geode::prelude;

// ---------- Settings helpers ----------

static bool opt(char const* key) { return Mod::get()->getSettingValue<bool>(key); }
static float optF(char const* key) { return static_cast<float>(Mod::get()->getSettingValue<double>(key)); }
static int optI(char const* key) { return static_cast<int>(Mod::get()->getSettingValue<int64_t>(key)); }
static std::string botMode() { return Mod::get()->getSettingValue<std::string>("bot-mode"); }

// ---------- Auto Safe: block saving whenever a cheat is active ----------

static bool cheatsActive() {
	return opt("noclip")
		|| (opt("speed-on") && std::fabs(optF("speed") - 1.f) > 0.001f)
		|| (opt("tps-on") && optI("tps") != 240)
		|| opt("jump-hack")
		|| opt("auto-complete")
		|| botMode() == "Replay";
}

// Auto complete ALWAYS blocks saving, regardless of the Auto Safe setting.
static bool blockSaving() {
	return opt("auto-complete") || (opt("auto-safe") && cheatsActive());
}

// ---------- Small utilities ----------

static ccColor3B hueToRGB(float h) {
	h = h - std::floor(h);
	float r = std::fabs(h * 6.f - 3.f) - 1.f;
	float g = 2.f - std::fabs(h * 6.f - 2.f);
	float b = 2.f - std::fabs(h * 6.f - 4.f);
	auto c = [](float v) { return static_cast<GLubyte>(std::clamp(v, 0.f, 1.f) * 255.f); };
	return { c(r), c(g), c(b) };
}

static void applyFPS() {
	if (opt("fps-on")) {
		CCApplication::sharedApplication()->setAnimationInterval(1.0 / optI("fps"));
	}
}

static FMOD::DSP* g_reverb = nullptr;

static void applyReverb(bool on) {
	auto engine = FMODAudioEngine::sharedEngine();
	if (!engine || !engine->m_system || !engine->m_backgroundMusicChannel) return;

	if (on && !g_reverb) {
		engine->m_system->createDSPByType(FMOD_DSP_TYPE_SFXREVERB, &g_reverb);
		engine->m_backgroundMusicChannel->addDSP(0, g_reverb);
	} else if (!on && g_reverb) {
		engine->m_backgroundMusicChannel->removeDSP(g_reverb);
		g_reverb->release();
		g_reverb = nullptr;
	}
}

// ---------- Bot (macro recorder / player) ----------

struct Action {
	int frame;
	bool down;
	int button;
	bool player1;
};

static std::vector<Action> g_actions;
static size_t g_idx = 0;
static bool g_fromBot = false;

static std::filesystem::path macroPath(GJGameLevel* level) {
	return Mod::get()->getSaveDir() / fmt::format("macro_{}.txt", level->m_levelID.value());
}

static void saveMacro(GJGameLevel* level) {
	std::ofstream f(macroPath(level));
	for (auto& a : g_actions) {
		f << a.frame << ' ' << a.down << ' ' << a.button << ' ' << a.player1 << '\n';
	}
}

static void loadMacro(GJGameLevel* level) {
	g_actions.clear();
	g_idx = 0;
	std::ifstream f(macroPath(level));
	Action a{};
	while (f >> a.frame >> a.down >> a.button >> a.player1) g_actions.push_back(a);
}

// ---------- Noclip death-limit bookkeeping ----------

static int g_deaths = 0;
static int g_lastHit = -10;

// ---------- Hooks: PlayLayer ----------

class $modify(HackPlayLayer, PlayLayer) {
	struct Fields {
		float hue = 0.f;
		int frames = 0;
	};

	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();

		applyFPS();
		g_deaths = 0;
		g_lastHit = -10;
		g_actions.clear();
		g_idx = 0;
		if (botMode() == "Replay") loadMacro(m_level);

		// tiny status label so you can see when saving is blocked
		auto label = CCLabelBMFont::create("", "bigFont.fnt");
		label->setID("hackpack-status");
		label->setScale(0.35f);
		label->setOpacity(150);
		label->setAnchorPoint({0.f, 1.f});
		label->setPosition({5.f, CCDirector::get()->getWinSize().height - 5.f});
		label->setZOrder(1000);
		this->addChild(label);
	}

	// Noclip (+ optional death limit)
	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (!opt("noclip") || object == m_anticheatSpike) {
			return PlayLayer::destroyPlayer(player, object);
		}

		if (opt("noclip-limit-on")) {
			int frame = m_gameState.m_currentProgress;
			if (frame - g_lastHit > 1) g_deaths++; // a new, separate hit
			g_lastHit = frame;
			if (g_deaths > optI("noclip-limit")) {
				return PlayLayer::destroyPlayer(player, object);
			}
		}
		// otherwise: swallow the death
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);

		// Rainbow icons
		if (opt("rainbow")) {
			m_fields->hue += dt * optF("rainbow-speed");
			auto main = hueToRGB(m_fields->hue);
			auto second = hueToRGB(m_fields->hue + 0.5f);
			for (auto p : {m_player1, m_player2}) {
				if (!p) continue;
				p->setColor(main);
				p->setSecondColor(second);
			}
		}

		// Speedhack
		float scale = opt("speed-on") ? optF("speed") : 1.f;
		CCDirector::get()->getScheduler()->setTimeScale(scale);

		// Hitboxes: make sure the debug draw node is shown
		if (opt("hitboxes") && m_debugDrawNode) m_debugDrawNode->setVisible(true);

		// Auto complete
		if (opt("auto-complete") && !m_levelEndAnimationStarted && ++m_fields->frames > 10) {
			this->levelComplete();
		}

		// Status label
		if (auto label = typeinfo_cast<CCLabelBMFont*>(this->getChildByID("hackpack-status"))) {
			label->setString(blockSaving() ? "Safe Mode" : "");
		}
	}

	void resetLevel() {
		PlayLayer::resetLevel();

		g_deaths = 0;
		g_lastHit = -10;

		// Practice music hack: sync music to the checkpoint position
		if (opt("practice-music") && m_isPracticeMode) {
			auto ms = static_cast<unsigned int>(m_gameState.m_levelTime * 1000.0);
			FMODAudioEngine::sharedEngine()->setMusicTimeMS(ms, true, 0);
		}

		// The music channel gets recreated on reset, so re-apply reverb
		g_reverb = nullptr;
		applyReverb(opt("reverb"));

		// Keep the macro in sync with checkpoints / restarts
		int frame = m_gameState.m_currentProgress;
		auto mode = botMode();
		if (mode == "Record") {
			std::erase_if(g_actions, [&](Action const& a) { return a.frame >= frame; });
		} else if (mode == "Replay") {
			g_idx = std::lower_bound(
				g_actions.begin(), g_actions.end(), frame,
				[](Action const& a, int f) { return a.frame < f; }
			) - g_actions.begin();
		}
	}

	void levelComplete() {
		if (botMode() == "Record") saveMacro(m_level);
		if (blockSaving()) m_isTestMode = true; // treated like a test run: nothing gets saved
		PlayLayer::levelComplete();
	}

	void onQuit() {
		if (botMode() == "Record") saveMacro(m_level);
		CCDirector::get()->getScheduler()->setTimeScale(1.f);
		g
