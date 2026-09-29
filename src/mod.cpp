#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/config.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include "d/actor/d_a_player.h"

#include "ma_fairy_companion.hpp"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ActorService, svc_actor);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

static bool    s_registered = false;
static bool    s_hasCompanion = false;
static ActorId s_companionId = 0;

// ---------------------------------------------------------------------------
// Mods panel: fairy color + light toggle
// ---------------------------------------------------------------------------

static ConfigVarHandle s_colorVar = 0;
static ConfigVarHandle s_lightVar = 0;
static ConfigVarHandle s_buttonVar = 0;

// 3 opciones a pedido. Si más adelante se quiere sumar el verde "extra"
// (FAIRY_COLOR_GREEN, ya definido en el enum), basta con agregar una línea
// acá.
static const char* k_fairyColorNames[] = {
    "Navi (Blue)",
    "Tatl (Orange)",
    "Tael (Purple)",
};
static constexpr size_t k_fairyColorCount =
sizeof(k_fairyColorNames) / sizeof(k_fairyColorNames[0]);

// Se disparan cuando el valor efectivo de la variable cambia en runtime,
// incluida la escritura que hacen los propios controles del panel
// (UI_BINDING_CONFIG_VAR).
static void onColorVarChanged(ModContext*, ConfigVarHandle, const ConfigVarValue* value,
    const ConfigVarValue*, void*) {
    maFairyCompanion_c::setSelectedColor((u8)value->int_value);
}

static void onLightVarChanged(ModContext*, ConfigVarHandle, const ConfigVarValue* value,
    const ConfigVarValue*, void*) {
    maFairyCompanion_c::setLightEnabled(value->bool_value);
}

// Solo Izquierda/Derecha: A/B/R chocan con acciones nativas mientras hay un
// enemigo fijado (ver la nota en ma_fairy_companion.cpp, Execute()).
static const char* k_fairyButtonNames[] = {
    "Pad-Left",
    "Pad-Right",
};
static constexpr size_t k_fairyButtonCount =
sizeof(k_fairyButtonNames) / sizeof(k_fairyButtonNames[0]);

static void onButtonVarChanged(ModContext*, ConfigVarHandle, const ConfigVarValue* value,
    const ConfigVarValue*, void*) {
    maFairyCompanion_c::setActionButton((u8)value->int_value);
}

static ModResult buildFairyPanel(ModContext* ctx, UiElementHandle pane, void*, ModError*) {
    UiControlDesc colorDesc = UI_CONTROL_DESC_INIT;
    colorDesc.kind = UI_CONTROL_DROPDOWN;
    colorDesc.label = "Fairy Color";
    colorDesc.binding = UI_BINDING_CONFIG_VAR;
    colorDesc.config_var = s_colorVar;
    colorDesc.options = k_fairyColorNames;
    colorDesc.option_count = k_fairyColorCount;
    ModResult r = svc_ui->pane_add_control(ctx, pane, &colorDesc, nullptr);
    if (r != MOD_OK) {
        return r;
    }

    UiControlDesc lightDesc = UI_CONTROL_DESC_INIT;
    lightDesc.kind = UI_CONTROL_TOGGLE;
    lightDesc.label = "Fairy Light";
    lightDesc.binding = UI_BINDING_CONFIG_VAR;
    lightDesc.config_var = s_lightVar;
    r = svc_ui->pane_add_control(ctx, pane, &lightDesc, nullptr);
    if (r != MOD_OK) {
        return r;
    }

    UiControlDesc buttonDesc = UI_CONTROL_DESC_INIT;
    buttonDesc.kind = UI_CONTROL_DROPDOWN;
    buttonDesc.label = "Action Button";
    buttonDesc.binding = UI_BINDING_CONFIG_VAR;
    buttonDesc.config_var = s_buttonVar;
    buttonDesc.options = k_fairyButtonNames;
    buttonDesc.option_count = k_fairyButtonCount;
    return svc_ui->pane_add_control(ctx, pane, &buttonDesc, nullptr);
}

extern "C" {

    MOD_EXPORT ModResult mod_initialize(ModError* outError) {
        if (svc_actor->register_actor(mod_ctx, &maFairyCompanion_c::sProfile,
            &maFairyCompanion_c::sProcName,
            &maFairyCompanion_c::sActorHandle) != MOD_OK)
        {
            svc_log->error(mod_ctx, "failed to register fairy companion actor");
            return MOD_ERROR;
        }
        s_registered = true;

        // --- Config var: color elegido, persistente en config.json ---
        ConfigVarDesc colorDesc = CONFIG_VAR_DESC_INIT;
        colorDesc.name = "fairy-color";
        colorDesc.type = CONFIG_VAR_INT;
        colorDesc.default_int = FAIRY_COLOR_BLUE;
        if (svc_config->register_var(mod_ctx, &colorDesc, &s_colorVar) != MOD_OK) {
            svc_log->error(mod_ctx, "failed to register fairy color config var");
            return MOD_ERROR;
        }
        svc_config->subscribe(mod_ctx, s_colorVar, onColorVarChanged, nullptr, nullptr);

        // --- Config var: luz del hada encendida/apagada ---
        ConfigVarDesc lightDesc = CONFIG_VAR_DESC_INIT;
        lightDesc.name = "fairy-light";
        lightDesc.type = CONFIG_VAR_BOOL;
        lightDesc.default_bool = true;
        if (svc_config->register_var(mod_ctx, &lightDesc, &s_lightVar) != MOD_OK) {
            svc_log->error(mod_ctx, "failed to register fairy light config var");
            return MOD_ERROR;
        }
        svc_config->subscribe(mod_ctx, s_lightVar, onLightVarChanged, nullptr, nullptr);

        // --- Config var: botón de acción elegido (Izquierda/Derecha) ---
        ConfigVarDesc buttonDesc = CONFIG_VAR_DESC_INIT;
        buttonDesc.name = "fairy-button";
        buttonDesc.type = CONFIG_VAR_INT;
        buttonDesc.default_int = FAIRY_BUTTON_LEFT;
        if (svc_config->register_var(mod_ctx, &buttonDesc, &s_buttonVar) != MOD_OK) {
            svc_log->error(mod_ctx, "failed to register fairy button config var");
            return MOD_ERROR;
        }
        svc_config->subscribe(mod_ctx, s_buttonVar, onButtonVarChanged, nullptr, nullptr);

        // Aplica de entrada los valores guardados de una partida anterior (o
        // los defaults si es la primera vez), en vez de esperar al primer
        // cambio del usuario en el menú.
        int64_t savedColor = FAIRY_COLOR_BLUE;
        svc_config->get_int(mod_ctx, s_colorVar, &savedColor);
        maFairyCompanion_c::setSelectedColor((u8)savedColor);

        bool savedLightEnabled = true;
        svc_config->get_bool(mod_ctx, s_lightVar, &savedLightEnabled);
        maFairyCompanion_c::setLightEnabled(savedLightEnabled);

        // setActionButton() itself clamps anything above FAIRY_BUTTON_RIGHT
        // (e.g. leftover 2/3 = old A/B saved by config.json before this
        // change) back to Left, so no extra validation needed here.
        int64_t savedButton = FAIRY_BUTTON_LEFT;
        svc_config->get_int(mod_ctx, s_buttonVar, &savedButton);
        maFairyCompanion_c::setActionButton((u8)savedButton);

        // --- Panel en la ventana de Mods ---
        UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
        panelDesc.build = buildFairyPanel;
        if (svc_ui->register_mods_panel(mod_ctx, &panelDesc) != MOD_OK) {
            svc_log->error(mod_ctx, "failed to register fairy mod panel");
            return MOD_ERROR;
        }

        svc_log->info(mod_ctx, "fairy companion mod initialized");
        return MOD_OK;
    }

    MOD_EXPORT ModResult mod_update(ModError*) {
        daPy_py_c* player = daPy_getPlayerActorClass();

        if (player == nullptr || !s_registered) {
            return MOD_OK;
        }

        // Liveness check every frame via ActorService instead of trusting that
        // the ActorId stays valid forever. Same reason we moved from raw pointers
        // to fpc_ProcID earlier: the game can destroy the actor on a scene change.
        int8_t roomNum = 0;
        bool alive = s_hasCompanion
            && (svc_actor->get_actor_room_num(mod_ctx, s_companionId, &roomNum) == MOD_OK);

        if (!alive) {
            if (s_hasCompanion) {
                svc_log->info(mod_ctx, "companion actor lost, respawning");
            }
            s_hasCompanion = false;

            ActorSpawnParams params{};
            params.parameters = 0;
            params.argument = 0;
            params.room_num = fopAcM_GetRoomNo(player);
            params.position = { player->current.pos.x,
                                  player->current.pos.y + 100.0f,
                                  player->current.pos.z };
            params.angle = { 0, 0, 0 };
            params.scale = { 1.0f, 1.0f, 1.0f };

            if (svc_actor->create_actor(mod_ctx, maFairyCompanion_c::sProcName,
                &params, &s_companionId) == MOD_OK)
            {
                s_hasCompanion = true;
                svc_log->info(mod_ctx, "fairy companion spawned");
            }
        }

        return MOD_OK;
    }

    MOD_EXPORT ModResult mod_shutdown(ModError*) {
        if (s_hasCompanion) {
            svc_actor->delete_actor(mod_ctx, s_companionId);
            s_hasCompanion = false;
        }

        if (s_registered) {
            svc_actor->unregister_actor(mod_ctx, maFairyCompanion_c::sActorHandle);
            s_registered = false;
        }

        // Config vars, subscriptions, and the mods panel are torn down
        // automatically by the host when the mod unloads.

        return MOD_OK;
    }

}  // extern "C"