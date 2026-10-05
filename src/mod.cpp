#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/resource.h"
#include "mods/svc/ui.h"
#include "mods/svc/audio_res.h"

#include "d/actor/d_a_player.h"
#include "m_Do/m_Do_audio.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "ma_fairy_companion.hpp"

#include <cstdio>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ResourceService, svc_resource);
IMPORT_SERVICE(UiService, svc_ui);
IMPORT_SERVICE(AudioResService, svc_audio_res);

// ---------------------------------------------------------------------------
// Companion state and configuration
// ---------------------------------------------------------------------------

static bool s_registered = false;
static bool s_hasCompanion = false;
static bool s_hudInitialized = false;
static ActorId s_companionId = 0;

static ConfigVarHandle s_colorVar = 0;
static ConfigVarHandle s_lightVar = 0;
static ConfigVarHandle s_buttonVar = 0;

static const char* k_fairyColorNames[] = {
    "Navi (Blue)",
    "Tatl (Orange)",
    "Tael (Purple)"
};

static const char* k_fairyButtonNames[] = {
    "Pad-Left",
    "Pad-Right"
};
// --------------------------------------------------------------------------------------
// ===== Sonidos del hada: intercambio temporal de dos ondas del hada de la botella =====
// --------------------------------------------------------------------------------------
static constexpr uint16_t k_sfxMainWave = 4557;   // onda principal del portador
static constexpr uint16_t k_sfxMuteWave = 4693;   // capa secundaria (se silencia)
static constexpr uint16_t k_sfxDummyWave = 9999;   // onda que nadie usa: solo fuerza la reconstruccion
static constexpr u32      k_sfxCarrierSe = Z2SE_FAIRY_S_FLY_RTT;
static constexpr float    k_sfxVolume = 1.0f;   // 0-2; bajalo si normalizaste el WAV
static constexpr int      k_sfxPlayDelay = 3;      // frames hasta disparar (frame_end aplica el cambio)
static constexpr int      k_sfxStopFrames = 45;   // ~1,5 s despues de disparar, se corta el portador
static constexpr int      k_sfxStopFade = 5;    // fundido del corte
static constexpr int      k_sfxHoldFrames = 90;   // ~3 s antes de restaurar
static constexpr bool     k_sfxVerifyOriginal = false;  // test: vuelve a sonar el original tras restaurar

static const char* const k_sfxFiles[2] = {
    "res/sounds/OOT_Navi_In.wav",    // se guarda en Link
    "res/sounds/OOT_Navi_Out.wav",   // sale
};

static AudioWaveHandle       s_sfxMain = 0, s_sfxMute = 0, s_sfxDummy = 0;
static AudioSoundTableHandle s_sfxFx = 0;
static int s_sfxPlayIn = 0, s_sfxRestoreIn = 0, s_sfxVerifyIn = 0;
static int s_sfxStopIn = 0;

static void sfxLog(const char* what, ModResult r) {
    char text[96];
    std::snprintf(text, sizeof(text), "FAIRY SFX: %s result=%d", what, static_cast<int>(r));
    //svc_log->info(mod_ctx, text);
}

// Quita lo instalado (no fuerza la reconstruccion)
static void sfxRelease() {
    if (s_sfxMain) { svc_audio_res->remove_wave(mod_ctx, s_sfxMain); s_sfxMain = 0; }
    if (s_sfxMute) { svc_audio_res->remove_wave(mod_ctx, s_sfxMute); s_sfxMute = 0; }
    if (s_sfxFx) { svc_audio_res->remove_sound_table(mod_ctx, s_sfxFx); s_sfxFx = 0; }
}

// Fuerza la reconstruccion del mapa de ondas con un reemplazo inofensivo
static void sfxPoke() {
    if (s_sfxDummy) { svc_audio_res->remove_wave(mod_ctx, s_sfxDummy); s_sfxDummy = 0; }
    svc_audio_res->replace_wave(mod_ctx, AUDIO_WAVE_BANK_SOUND_EFFECTS, k_sfxDummyWave,
        "res/sounds/silence.wav", svc_audio_res->default_wave_info,
        &s_sfxDummy);
}

static void sfxRestore() {
    sfxRelease();
    sfxPoke();
    s_sfxPlayIn = s_sfxRestoreIn = 0;
    s_sfxStopIn = 0;
    //svc_log->info(mod_ctx, "FAIRY SFX: restaurado");
    //if (k_sfxVerifyOriginal) {
    //    s_sfxVerifyIn = 60;
    //}
}

void maFairySfx_play(bool hiding) {
    mDoAud_seStop(k_sfxCarrierSe, 0);     // corta el portador anterior si sigue sonando
    sfxRelease();   // por si habia un intercambio activo; el replace de abajo fuerza la reconstruccion

    sfxLog("main", svc_audio_res->replace_wave(mod_ctx, AUDIO_WAVE_BANK_SOUND_EFFECTS,
        k_sfxMainWave, k_sfxFiles[hiding ? 0 : 1], svc_audio_res->default_wave_info, &s_sfxMain));
    sfxLog("mute", svc_audio_res->replace_wave(mod_ctx, AUDIO_WAVE_BANK_SOUND_EFFECTS,
        k_sfxMuteWave, "res/sounds/silence.wav", svc_audio_res->default_wave_info, &s_sfxMute));

    // Volumen y sin atenuacion por distancia (Z2SE 0x600D8 = categoria CHARA_SE, efecto 0xD8)
    AudioSoundTableEffectInfo fx = *svc_audio_res->default_effect_info;
    fx.volume = k_sfxVolume;
    fx.ignore_distance_volume = true;
    fx.ignore_distance_fx_mix = true;
    sfxLog("fx", svc_audio_res->replace_sound_table_effect(mod_ctx, SE_CATEGORY_CHARA_SE, 0xD8,
        &fx, &s_sfxFx));

    s_sfxPlayIn = k_sfxPlayDelay;
    s_sfxStopIn = k_sfxPlayDelay + k_sfxStopFrames;
    s_sfxRestoreIn = k_sfxHoldFrames;
    //s_sfxVerifyIn = 0;
}

static void sfxTick() {
    if (s_sfxPlayIn > 0 && --s_sfxPlayIn == 0) {
        mDoAud_seStartMenu(k_sfxCarrierSe);
    }
    if (s_sfxStopIn > 0 && --s_sfxStopIn == 0) { 
        mDoAud_seStop(k_sfxCarrierSe, k_sfxStopFade);
    }
    if (s_sfxRestoreIn > 0 && --s_sfxRestoreIn == 0) {
        sfxRestore();
    }
    /*if (s_sfxVerifyIn > 0 && --s_sfxVerifyIn == 0) {
        svc_log->info(mod_ctx, "FAIRY SFX: verificacion, suena el hada ORIGINAL ahora");
        mDoAud_seStartMenu(k_sfxCarrierSe);
    }*/
}

// ---------------------------------------------------------------------------
// Configuration callbacks
// ---------------------------------------------------------------------------

static void onColorVarChanged(
    ModContext*, ConfigVarHandle,
    const ConfigVarValue* value,
    const ConfigVarValue*, void*) {

    maFairyCompanion_c::setSelectedColor(
        static_cast<u8>(value->int_value)
    );
}

static void onLightVarChanged(
    ModContext*, ConfigVarHandle,
    const ConfigVarValue* value,
    const ConfigVarValue*, void*) {

    maFairyCompanion_c::setLightEnabled(
        value->bool_value
    );
}

static void onButtonVarChanged(
    ModContext*, ConfigVarHandle,
    const ConfigVarValue* value,
    const ConfigVarValue*, void*) {

    maFairyCompanion_c::setActionButton(
        static_cast<u8>(value->int_value)
    );
}

// ---------------------------------------------------------------------------
// Settings panel
// ---------------------------------------------------------------------------

static ModResult buildFairyPanel(
    ModContext* ctx, UiElementHandle pane,
    void*, ModError*) {

    UiControlDesc color = UI_CONTROL_DESC_INIT;
    color.kind = UI_CONTROL_DROPDOWN;
    color.label = "Fairy Color";
    color.binding = UI_BINDING_CONFIG_VAR;
    color.config_var = s_colorVar;
    color.options = k_fairyColorNames;
    color.option_count =
        sizeof(k_fairyColorNames) / sizeof(k_fairyColorNames[0]);

    ModResult result = svc_ui->pane_add_control(
        ctx, pane, &color, nullptr
    );

    if (result != MOD_OK) {
        return result;
    }

    UiControlDesc light = UI_CONTROL_DESC_INIT;
    light.kind = UI_CONTROL_TOGGLE;
    light.label = "Fairy Light";
    light.binding = UI_BINDING_CONFIG_VAR;
    light.config_var = s_lightVar;

    result = svc_ui->pane_add_control(
        ctx, pane, &light, nullptr
    );

    if (result != MOD_OK) {
        return result;
    }

    UiControlDesc button = UI_CONTROL_DESC_INIT;
    button.kind = UI_CONTROL_DROPDOWN;
    button.label = "Action Button";
    button.binding = UI_BINDING_CONFIG_VAR;
    button.config_var = s_buttonVar;
    button.options = k_fairyButtonNames;
    button.option_count =
        sizeof(k_fairyButtonNames) / sizeof(k_fairyButtonNames[0]);

    return svc_ui->pane_add_control(
        ctx, pane, &button, nullptr
    );
}

// ---------------------------------------------------------------------------
// Mod lifecycle
// ---------------------------------------------------------------------------

extern "C" {

    MOD_EXPORT ModResult mod_initialize(ModError*) {
        // Register the unchanged companion actor profile.
        if (svc_actor->register_actor(
            mod_ctx,
            &maFairyCompanion_c::sProfile,
            &maFairyCompanion_c::sProcName,
            &maFairyCompanion_c::sActorHandle) != MOD_OK) {

            svc_log->error(
                mod_ctx, "failed to register fairy companion actor"
            );
            return MOD_ERROR;
        }

        s_registered = true;

        // Register color preference.
        ConfigVarDesc color = CONFIG_VAR_DESC_INIT;
        color.name = "fairy-color";
        color.type = CONFIG_VAR_INT;
        color.default_int = FAIRY_COLOR_BLUE;

        if (svc_config->register_var(
            mod_ctx, &color, &s_colorVar) != MOD_OK) {
            return MOD_ERROR;
        }

        svc_config->subscribe(
            mod_ctx, s_colorVar,
            onColorVarChanged, nullptr, nullptr
        );

        // Register light preference.
        ConfigVarDesc light = CONFIG_VAR_DESC_INIT;
        light.name = "fairy-light";
        light.type = CONFIG_VAR_BOOL;
        light.default_bool = true;

        if (svc_config->register_var(
            mod_ctx, &light, &s_lightVar) != MOD_OK) {
            return MOD_ERROR;
        }

        svc_config->subscribe(
            mod_ctx, s_lightVar,
            onLightVarChanged, nullptr, nullptr
        );

        // Register action button preference.
        ConfigVarDesc button = CONFIG_VAR_DESC_INIT;
        button.name = "fairy-button";
        button.type = CONFIG_VAR_INT;
        button.default_int = FAIRY_BUTTON_LEFT;

        if (svc_config->register_var(
            mod_ctx, &button, &s_buttonVar) != MOD_OK) {
            return MOD_ERROR;
        }

        svc_config->subscribe(
            mod_ctx, s_buttonVar,
            onButtonVarChanged, nullptr, nullptr
        );

        // Restore saved values.
        int64_t savedColor = FAIRY_COLOR_BLUE;
        svc_config->get_int(
            mod_ctx, s_colorVar, &savedColor
        );
        maFairyCompanion_c::setSelectedColor(
            static_cast<u8>(savedColor)
        );

        bool savedLight = true;
        svc_config->get_bool(
            mod_ctx, s_lightVar, &savedLight
        );
        maFairyCompanion_c::setLightEnabled(savedLight);

        int64_t savedButton = FAIRY_BUTTON_LEFT;
        svc_config->get_int(
            mod_ctx, s_buttonVar, &savedButton
        );
        maFairyCompanion_c::setActionButton(
            static_cast<u8>(savedButton)
        );

        // Register settings panel and original HUD.
        UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
        panel.build = buildFairyPanel;

        if (svc_ui->register_mods_panel(
            mod_ctx, &panel) != MOD_OK) {
            return MOD_ERROR;
        }

        sfxPoke();   // registra la onda de relleno

        maFairyHud_init();
        s_hudInitialized = true;

        svc_log->info(
            mod_ctx, "fairy companion mod initialized"
        );

        return MOD_OK;
    }

    MOD_EXPORT ModResult mod_update(ModError*) {
        sfxTick();
        daPy_py_c* player = daPy_getPlayerActorClass();

        if (player == nullptr || !s_registered) {
            return MOD_OK;
        }

        // Check whether the companion remains available.
        int8_t roomNum = 0;
        const bool alive =
            s_hasCompanion &&
            svc_actor->get_actor_room_num(
                mod_ctx, s_companionId, &roomNum) == MOD_OK;

        if (!alive) {
            if (s_hasCompanion) {
                svc_log->info(
                    mod_ctx, "companion actor lost, respawning"
                );
            }

            s_hasCompanion = false;

            // Spawn near Link.
            ActorSpawnParams params{};
            params.parameters = 0;
            params.argument = 0;
            params.room_num = fopAcM_GetRoomNo(player);
            params.position = {
                player->current.pos.x,
                player->current.pos.y + 100.0f,
                player->current.pos.z
            };
            params.angle = { 0, 0, 0 };
            params.scale = { 1.0f, 1.0f, 1.0f };

            if (svc_actor->create_actor(
                mod_ctx,
                maFairyCompanion_c::sProcName,
                &params,
                &s_companionId) == MOD_OK) {

                s_hasCompanion = true;
                svc_log->info(
                    mod_ctx, "fairy companion spawned"
                );
            }
        }

        return MOD_OK;
    }


    MOD_EXPORT ModResult mod_shutdown(ModError*) {
        sfxRestore();


        if (s_hasCompanion) {
            svc_actor->delete_actor(
                mod_ctx, s_companionId
            );
            s_hasCompanion = false;
        }

        if (s_registered) {
            svc_actor->unregister_actor(
                mod_ctx, maFairyCompanion_c::sActorHandle
            );
            s_registered = false;
        }

        if (s_hudInitialized) {
            maFairyHud_shutdown();
            s_hudInitialized = false;
        }

        return MOD_OK;
    }

} // extern "C"