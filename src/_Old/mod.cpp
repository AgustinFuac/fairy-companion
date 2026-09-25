#include "mods/service.hpp"
#include "mods/svc/actor.h"
#include "mods/svc/log.h"

#include "d/actor/d_a_player.h"

#include "ma_fairy_companion.hpp"

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(ActorService, svc_actor);

static bool s_registered = false;
static bool s_hasCompanion = false;
static ActorId s_companionId = 0;

extern "C" {

    MOD_EXPORT ModResult mod_initialize(ModError*) {
        if (svc_actor->register_actor(mod_ctx, &maFairyCompanion_c::sProfile,
            &maFairyCompanion_c::sProcName, &maFairyCompanion_c::sActorHandle) != MOD_OK)
        {
            svc_log->error(mod_ctx, "failed to register fairy companion actor");
            return MOD_ERROR;
        }

        s_registered = true;
        svc_log->info(mod_ctx, "fairy companion mod initialized");
        return MOD_OK;
    }

    MOD_EXPORT ModResult mod_update(ModError*) {
        daPy_py_c* player = daPy_getPlayerActorClass();

        if (player == nullptr || !s_registered) {
            return MOD_OK;
        }

        // Comprobamos cada frame si el actor sigue vivo via ActorService, en vez
        // de confiar en que el ActorId siga siendo valido para siempre (mismo
        // motivo que nos hizo pasar de punteros crudos a fpc_ProcID mas atras:
        // el actor puede desaparecer al cambiar de escena).
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
            params.position = {
                player->current.pos.x, player->current.pos.y + 100.0f, player->current.pos.z };
            params.angle = { 0, 0, 0 };
            params.scale = { 1.0f, 1.0f, 1.0f };

            if (svc_actor->create_actor(mod_ctx, maFairyCompanion_c::sProcName, &params, &s_companionId)
                == MOD_OK)
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

        return MOD_OK;
    }

}