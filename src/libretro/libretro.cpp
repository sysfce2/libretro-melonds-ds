/*
    Copyright 2023 Jesse Talavera-Greenberg

    melonDS DS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS DS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS DS. If not, see http://www.gnu.org/licenses/.
*/

#include "libretro.hpp"

// NOT UNUSED; GPU.h doesn't #include OpenGL, so I do it here.
// This must come before <GPU.h>!
#include "PlatformOGLPrivate.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <span>

#include <compat/strl.h>
#include <file/file_path.h>
#include <libretro.h>
#include <retro_assert.h>
#include <retro_miscellaneous.h>

#undef isnan
#include <fmt/format.h>

#include "config/config.hpp"
#include "core/core.hpp"
#include "environment.hpp"
#include "exceptions.hpp"
#include "info.hpp"
#include "retro/task_queue.hpp"
#include "sram.hpp"
#include "tracy.hpp"
#include "version.hpp"

using namespace melonDS;
using std::make_optional;
using std::optional;
using std::nullopt;
using std::span;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::make_unique;
using retro::task::TaskSpec;

namespace MelonDsDs {
    // Aligned with CoreState to prevent undefined behavior
    alignas(CoreState) static std::array<std::byte, sizeof(CoreState)> CoreStateBuffer;
    CoreState& Core = *reinterpret_cast<CoreState*>(CoreStateBuffer.data());
}

PUBLIC_SYMBOL void retro_init(void) {
#ifdef HAVE_TRACY
    tracy::StartupProfiler();
#endif
    TracySetProgramName(MELONDSDS_VERSION);
    ZoneScopedN(TracyFunction);
    retro::env::init();
    retro::debug("retro_init");
    retro::info("{} {}", MELONDSDS_NAME, MELONDSDS_VERSION);
    retro_assert(!MelonDsDs::Core.IsInitialized());

    retro::task::init(false, nullptr);

    memset(MelonDsDs::CoreStateBuffer.data(), 0, MelonDsDs::CoreStateBuffer.size());
    new(&MelonDsDs::CoreStateBuffer) MelonDsDs::CoreState(); // placement-new the CoreState
    retro_assert(MelonDsDs::Core.IsInitialized());
}

PUBLIC_SYMBOL bool retro_load_game(const struct retro_game_info *info) {
    ZoneScopedN(TracyFunction);
    if (info) {
        ZoneText(info->path, strlen(info->path));
        retro::debug("retro_load_game(\"{}\", {})", info->path ? info->path : "", info->size);
    }
    else {
        retro::debug("retro_load_game(<no content>)");
    }

    std::span<const retro_game_info> content = info ? std::span(info, 1) : std::span<const retro_game_info>();

    return MelonDsDs::Core.LoadGame(MelonDsDs::MELONDSDS_GAME_TYPE_NDS, content);
}

PUBLIC_SYMBOL void retro_get_system_av_info(struct retro_system_av_info *info) {
    ZoneScopedN(TracyFunction);
    retro::debug(TracyFunction);

    retro_assert(info != nullptr);

    *info = MelonDsDs::Core.GetSystemAvInfo();

    retro::debug("retro_get_system_av_info finished");
}

PUBLIC_SYMBOL void retro_set_controller_port_device(unsigned port, unsigned device) {
    MelonDsDs::Core.GetInputState().SetControllerPortDevice(port, device);
}

PUBLIC_SYMBOL [[gnu::hot]] void retro_run(void) {
    {
        ZoneScopedN(TracyFunction);
        MelonDsDs::Core.Run();
    }
    FrameMark;
}

PUBLIC_SYMBOL void retro_unload_game(void) {
    ZoneScopedN(TracyFunction);
    using MelonDsDs::Core;
    retro::debug("retro_unload_game()");
    // No need to flush SRAM to the buffer, Platform::WriteNDSSave has been doing that for us this whole time
    // No need to flush the homebrew save data either, the CartHomebrew destructor does that

    // The cleanup handlers for each task will flush data to disk if needed
    retro::task::reset();
    retro::task::wait();
    retro::task::deinit();

    Core.UnloadGame();
}

PUBLIC_SYMBOL unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

PUBLIC_SYMBOL bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
    ZoneScopedN(TracyFunction);
    retro::debug("retro_load_game_special({}, {}, {})", MelonDsDs::get_game_type_name(type), fmt::ptr(info), num);

    return MelonDsDs::Core.LoadGame(type, std::span(info, num));
}

// We deinitialize all these variables just in case the frontend doesn't unload the dynamic library.
// It might be keeping the library around for debugging purposes,
// or it might just be buggy.
PUBLIC_SYMBOL void retro_deinit(void) {
    { // Scoped so that we can capture one last scope before shutting down the profiler
        ZoneScopedN(TracyFunction);
        retro::debug("retro_deinit()");
        retro::task::deinit();
        MelonDsDs::Core.~CoreState(); // placement delete
        memset(MelonDsDs::CoreStateBuffer.data(), 0, MelonDsDs::CoreStateBuffer.size());
        retro_assert(!MelonDsDs::Core.IsInitialized());
        retro::env::deinit();
    }

#ifdef HAVE_TRACY
    tracy::ShutdownProfiler();
#endif
}

PUBLIC_SYMBOL unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

PUBLIC_SYMBOL void retro_get_system_info(struct retro_system_info *info) {
    info->library_name = MELONDSDS_NAME;
    info->block_extract = false;
    info->library_version = MELONDSDS_VERSION;
    info->need_fullpath = false;
    info->valid_extensions = "nds|ids|dsi";
}

PUBLIC_SYMBOL void retro_reset(void) {
    ZoneScopedN(TracyFunction);
    retro::debug("retro_reset()\n");

    try {
        MelonDsDs::Core.Reset();
    }
    catch (const MelonDsDs::opengl_exception& e) {
        retro::error("{}", e.what());
        retro::set_error_message(e.user_message());
        retro::shutdown();
        // TODO: Instead of shutting down, fall back to the software renderer
    }
    catch (const MelonDsDs::emulator_exception& e) {
        retro::error("{}", e.what());
        retro::set_error_message(e.user_message());
        retro::shutdown();
    }
    catch (const std::exception& e) {
        retro::set_error_message(e.what());
        retro::shutdown();
    }
    catch (...) {
        retro::set_error_message("An unknown error has occurred.");
        retro::shutdown();
    }
}

PUBLIC_SYMBOL void retro_cheat_reset(void) {
    ZoneScopedN(TracyFunction);

    MelonDsDs::Core.CheatReset();
}

PUBLIC_SYMBOL void retro_cheat_set(unsigned index, bool enabled, const char *code) {
    // Cheat codes are small programs, so we can't exactly turn them off (that would be undoing them)
    ZoneScopedN(TracyFunction);

    MelonDsDs::Core.CheatSet(index, enabled, code);
}

static const char *memory_type_name(unsigned type)
{
    switch (type) {
        case RETRO_MEMORY_SAVE_RAM:
            return "RETRO_MEMORY_SAVE_RAM";
        case RETRO_MEMORY_RTC:
            return "RETRO_MEMORY_RTC";
        case RETRO_MEMORY_SYSTEM_RAM:
            return "RETRO_MEMORY_SYSTEM_RAM";
        case RETRO_MEMORY_VIDEO_RAM:
            return "RETRO_MEMORY_VIDEO_RAM";
        case MelonDsDs::MELONDSDS_MEMORY_GBA_SAVE_RAM:
            return "MELONDSDS_MEMORY_GBA_SAVE_RAM";
        default:
            return "<unknown>";
    }
}

PUBLIC_SYMBOL size_t retro_serialize_size(void) {
    ZoneScopedN(TracyFunction);

    return MelonDsDs::Core.SerializeSize();
}

PUBLIC_SYMBOL bool retro_serialize(void *data, size_t size) {
    ZoneScopedN(TracyFunction);

    return MelonDsDs::Core.Serialize(std::span(static_cast<std::byte*>(data), size));
}

PUBLIC_SYMBOL bool retro_unserialize(const void *data, size_t size) {
    ZoneScopedN(TracyFunction);
    retro::debug("retro_unserialize({}, {})", data, size);

    return MelonDsDs::Core.Unserialize(std::span(static_cast<const std::byte*>(data), size));
}

PUBLIC_SYMBOL void *retro_get_memory_data(unsigned type) {
    ZoneScopedN(TracyFunction);
    retro::debug("retro_get_memory_data({})\n", memory_type_name(type));

    return MelonDsDs::Core.GetMemoryData(type);
}

PUBLIC_SYMBOL size_t retro_get_memory_size(unsigned type) {
    ZoneScopedN(TracyFunction);

    return MelonDsDs::Core.GetMemorySize(type);
}

extern "C" void MelonDsDs::HardwareContextReset() noexcept {
    try {
        Core.ResetRenderState();
    }
    catch (const opengl_exception& e) {
        retro::error("{}", e.what());
        retro::set_error_message(e.user_message());
        retro::shutdown();
        // TODO: Instead of shutting down, fall back to the software renderer
    }
    catch (const emulator_exception& e) {
        retro::error("{}", e.what());
        retro::set_error_message(e.user_message());
        retro::shutdown();
    }
    catch (const std::exception& e) {
        retro::set_error_message(e.what());
        retro::shutdown();
    }
    catch (...) {
        retro::set_error_message("OpenGL context initialization failed with an unknown error. Please report this issue.");
        retro::shutdown();
    }
}

extern "C" void MelonDsDs::HardwareContextDestroyed() noexcept {
    Core.DestroyRenderState();
}

extern "C" bool MelonDsDs::UpdateOptionVisibility() noexcept {
    return Core.UpdateOptionVisibility();
}

int Platform::Net_SendPacket(u8* data, int len, void*) {
    ZoneScopedN(TracyFunction);

    return MelonDsDs::Core.LanSendPacket(std::span((std::byte*)data, len));
}

int Platform::Net_RecvPacket(u8* data, void*) {
    ZoneScopedN(TracyFunction);

    return MelonDsDs::Core.LanRecvPacket(data);
}

void Platform::WriteNDSSave(const u8 *savedata, u32 savelen, u32 writeoffset, u32 writelen, void*) {
    ZoneScopedN(TracyFunction);

    MelonDsDs::Core.WriteNdsSave(span((const std::byte*)savedata, savelen), writeoffset, writelen);
}

void Platform::WriteGBASave(const u8 *savedata, u32 savelen, u32 writeoffset, u32 writelen, void*) {
    ZoneScopedN(TracyFunction);

    MelonDsDs::Core.WriteGbaSave(span((const std::byte*)savedata, savelen), writeoffset, writelen);
}

void Platform::WriteFirmware(const Firmware& firmware, u32 writeoffset, u32 writelen, void*) {
    ZoneScopedN(TracyFunction);

    MelonDsDs::Core.WriteFirmware(firmware, writeoffset, writelen);
}

extern "C" void MelonDsDs::MpStarted(uint16_t client_id, retro_netpacket_send_t send_fn, retro_netpacket_poll_receive_t poll_receive_fn) noexcept {
    MelonDsDs::Core.MpStarted(send_fn, poll_receive_fn);
}

extern "C" void MelonDsDs::MpReceived(const void* buf, size_t len, uint16_t client_id) noexcept {
    MelonDsDs::Core.MpPacketReceived(buf, len, client_id);
}

extern "C" void MelonDsDs::MpStopped() noexcept {
    MelonDsDs::Core.MpStopped();
}

int DeconstructPacket(u8 *data, u64 *timestamp, const std::optional<MelonDsDs::Packet> &o_p) {
    if (!o_p.has_value()) {
        return 0;
    }
    memcpy(data, o_p->Data(), o_p->Length());
    *timestamp = o_p->Timestamp();
    return o_p->Length();
}

int Platform::MP_SendPacket(u8* data, int len, u64 timestamp, void*) {
    return MelonDsDs::Core.MpSendPacket(MelonDsDs::Packet(data, len, timestamp, 0, MelonDsDs::Packet::Type::Other)) ? len : 0;
}

int Platform::MP_RecvPacket(u8* data, u64* timestamp, void*) {
    std::optional<MelonDsDs::Packet> o_p = MelonDsDs::Core.MpNextPacket();
    return DeconstructPacket(data, timestamp, o_p);
}

int Platform::MP_SendCmd(u8* data, int len, u64 timestamp, void*) {
    return MelonDsDs::Core.MpSendPacket(MelonDsDs::Packet(data, len, timestamp, 0, MelonDsDs::Packet::Type::Cmd)) ? len : 0;
}

int Platform::MP_SendReply(u8 *data, int len, u64 timestamp, u16 aid, void*) {
    // aid is always less than 16,
    // otherwise sending a 16-bit wide aidmask in RecvReplies wouldn't make sense,
    // and neither would this line[1] from melonDS itself.
    // A blog post from melonDS[2] from 2017 also confirms that
    // "each client is given an ID from 1 to 15"
    // [1] https://github.com/melonDS-emu/melonDS/blob/817b409ec893fb0b2b745ee18feced08706419de/src/net/LAN.cpp#L1074
    // [2] https://melonds.kuribo64.net/comments.php?id=25
    retro_assert(aid < 16);
    return MelonDsDs::Core.MpSendPacket(MelonDsDs::Packet(data, len, timestamp, aid, MelonDsDs::Packet::Type::Reply)) ? len : 0;
}

int Platform::MP_SendAck(u8* data, int len, u64 timestamp, void*) {
    return MelonDsDs::Core.MpSendPacket(MelonDsDs::Packet(data, len, timestamp, 0, MelonDsDs::Packet::Type::Cmd)) ? len : 0;
}

int Platform::MP_RecvHostPacket(u8* data, u64 * timestamp, void*) {
    std::optional<MelonDsDs::Packet> o_p = MelonDsDs::Core.MpNextPacketBlock();
    return DeconstructPacket(data, timestamp, o_p);
}

u16 Platform::MP_RecvReplies(u8* packets, u64 timestamp, u16 aidmask, void*) {
    if(!MelonDsDs::Core.MpActive()) {
        return 0;
    }
    u16 ret = 0;
    int loops = 0;
    while((ret & aidmask) != aidmask) {
        std::optional<MelonDsDs::Packet> o_p = MelonDsDs::Core.MpNextPacketBlock();
        if(!o_p.has_value()) {
            return ret;
        }
        MelonDsDs::Packet p = std::move(o_p).value();
        if(p.Timestamp() < (timestamp - 32)) {
            continue;
        }
        if(p.PacketType() != MelonDsDs::Packet::Type::Reply) {
            continue;
        }
        ret |= 1<<p.Aid();
        memcpy(&packets[(p.Aid()-1)*1024], p.Data(), std::min(p.Length(), (uint64_t)1024));
        loops++;
    }
    return ret;
}
