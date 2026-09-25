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
// Fairy color selector (mods panel)
// ---------------------------------------------------------------------------

static ConfigVarHandle s_colorVar = 0;

// 3 opciones a pedido. Si más adelante se quiere sumar el verde "extra"
// (FAIRY_COLOR_GREEN, ya definido en el enum), basta con agregar una línea
// acá.
static const char* k_fairyColorNames[] = {
    "Navi (Blue)",
    "Talt (Orange)",
    "Teal (Red)",
};
static constexpr size_t k_fairyColorCount =
sizeof(k_fairyColorNames) / sizeof(k_fairyColorNames[0]);

// Se dispara cuando el valor efectivo de la variable cambia en runtime,
// incluida la escritura que hace el propio dropdown (UI_BINDING_CONFIG_VAR).
static void onColorVarChanged(ModContext*, ConfigVarHandle, const ConfigVarValue* value,
    const ConfigVarValue*, void*) {
    maFairyCompanion_c::setSelectedColor((u8)value->int_value);
}

static ModResult buildFairyPanel(ModContext* ctx, UiElementHandle pane, void*, ModError*) {
    UiControlDesc desc = UI_CONTROL_DESC_INIT;
    desc.kind = UI_CONTROL_DROPDOWN;
    desc.label = "Fairy Color";
    desc.binding = UI_BINDING_CONFIG_VAR;
    desc.config_var = s_colorVar;
    desc.options = k_fairyColorNames;
    desc.option_count = k_fairyColorCount;
    return svc_ui->pane_add_control(ctx, pane, &desc, nullptr);
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

        // Aplica de entrada el valor guardado de una partida anterior (o el
        // default si es la primera vez), en vez de esperar al primer cambio
        // del usuario en el menú.
        int64_t savedColor = FAIRY_COLOR_BLUE;
        svc_config->get_int(mod_ctx, s_colorVar, &savedColor);
        maFairyCompanion_c::setSelectedColor((u8)savedColor);

        // --- Panel en la ventana de Mods, con el dropdown de color ---
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

        // Config var subscription and the mods panel are torn down
        // automatically by the host when the mod unloads.

        return MOD_OK;
    }

}  // extern "C"