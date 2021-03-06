/*
 * This file is part of EasyRPG Player.
 *
 * EasyRPG Player is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EasyRPG Player is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with EasyRPG Player. If not, see <http://www.gnu.org/licenses/>.
 */

#include "system.h"
#include "decoder_fluidsynth.h"

#if defined(HAVE_FLUIDSYNTH) || defined(HAVE_FLUIDLITE)

#include <cassert>
#include "filefinder.h"
#include "output.h"

#ifdef HAVE_FLUIDSYNTH
static void* vio_open(const char* filename) {
#else
static void* vio_open(fluid_fileapi_t*, const char* filename) {
#endif
	auto is = FileFinder::OpenInputStream(filename);
	return new Filesystem_Stream::InputStream { std::move(is) };
}

static int vio_read(void *ptr, int count, void* userdata) {
	auto* f = reinterpret_cast<Filesystem_Stream::InputStream*>(userdata);
	return f->read(reinterpret_cast<char*>(ptr), count).gcount();
}

static int vio_seek(void* userdata, long offset, int origin) {
	auto* f = reinterpret_cast<Filesystem_Stream::InputStream*>(userdata);
	if (f->eof()) f->clear(); // emulate behaviour of fseek

	f->seekg(offset, Filesystem_Stream::CSeekdirToCppSeekdir(origin));

	return f->tellg();
}

static long vio_tell(void* userdata) {
	auto* f = reinterpret_cast<Filesystem_Stream::InputStream*>(userdata);
	return f->tellg();
}

static int vio_close(void* userdata) {
	auto stream_ref = reinterpret_cast<Filesystem_Stream::InputStream*>(userdata);
	delete stream_ref;

	return 0;
}

#ifdef HAVE_FLUIDLITE
static fluid_fileapi_t fluidlite_vio = {
	nullptr,
	nullptr,
	vio_open,
	vio_read,
	vio_seek,
	vio_close,
	vio_tell
};
#endif

struct FluidSettingsDeleter {
	void operator()(fluid_settings_t* s) const {
		delete_fluid_settings(s);
	}
};

struct FluidSynthDeleter {
	void operator()(fluid_synth_t* s) const {
		delete_fluid_synth(s);
	}
};

namespace {
	std::unique_ptr<fluid_settings_t, FluidSettingsDeleter> global_settings;
	std::unique_ptr<fluid_synth_t, FluidSynthDeleter> global_synth;
#ifdef HAVE_FLUIDSYNTH
	fluid_sfloader_t* global_loader; // owned by global_settings
#endif
	int instances = 0;
}

static fluid_synth_t* create_synth(std::string& error_message) {
	fluid_synth_t* syn = new_fluid_synth(global_settings.get());

#ifdef HAVE_FLUIDSYNTH
	fluid_synth_add_sfloader(syn, global_loader);
#endif

	if (fluid_synth_sfload(syn, "easyrpg.soundfont", 1) == FLUID_FAILED) {
		error_message = "Could not load soundfont.";
		return nullptr;
	}

	fluid_synth_set_interp_method(syn, -1, FLUID_INTERP_LINEAR);

	return syn;
}

FluidSynthDecoder::FluidSynthDecoder() {
	++instances;

	// Optimisation: Only create the soundfont once and share the synth
	// Sharing is only not possible when a Midi is played as a SE (unlikely)
	if (instances > 1) {
		std::string error_message;
		instance_synth = create_synth(error_message);
		if (!instance_synth) {
			// unlikely, the SF was already allocated once
			Output::Debug("FluidSynth failed: {}", error_message);
		}
	} else {
		instance_synth = global_synth.get();
		fluid_synth_program_reset(global_synth.get());
	}
}

FluidSynthDecoder::~FluidSynthDecoder() {
	--instances;
	assert(instances >= 0);

	if (instance_synth != global_synth.get()) {
		delete_fluid_synth(instance_synth);
	}
}

bool FluidSynthDecoder::Initialize(std::string& error_message) {
	static bool init = false;
	static bool once = false;

	// only initialize once
	if (once)
		return init;
	once = true;

#ifdef HAVE_FLUIDLITE
	fluid_set_default_fileapi(&fluidlite_vio);
#endif

	if (!global_settings) {
		global_settings.reset(new_fluid_settings());
		fluid_settings_setstr(global_settings.get(), "player.timing-source", "sample");
		fluid_settings_setint(global_settings.get(), "synth.lock-memory", 0);

		fluid_settings_setnum(global_settings.get(), "synth.gain", 0.6);
		fluid_settings_setnum(global_settings.get(), "synth.sample-rate", EP_MIDI_FREQ);
		fluid_settings_setint(global_settings.get(), "synth.polyphony", 256);

#ifdef HAVE_FLUIDSYNTH
		fluid_settings_setint(global_settings.get(), "synth.reverb.active", 0);
		fluid_settings_setint(global_settings.get(), "synth.chorus.active", 0);
#else
		fluid_settings_setstr(global_settings.get(), "synth.reverb.active", "no");
		fluid_settings_setstr(global_settings.get(), "synth.chorus.active", "no");
#endif

#ifdef HAVE_FLUIDSYNTH
		// owned by fluid_settings
		global_loader = new_fluid_defsfloader(global_settings.get());
		fluid_sfloader_set_callbacks(global_loader,
				vio_open, vio_read, vio_seek, vio_tell, vio_close);
#endif
	}

	if (!global_synth) {
		global_synth.reset(create_synth(error_message));
		if (!global_synth) {
			return false;
		}
	}

	init = true;

	return init;
}

int FluidSynthDecoder::FillBuffer(uint8_t* buffer, int length) {
	if (!instance_synth) {
		return -1;
	}

	if (fluid_synth_write_s16(instance_synth, length / 4, buffer, 0, 2, buffer, 1, 2) == FLUID_FAILED) {
		return -1;
	}

	return length;
}

void FluidSynthDecoder::OnMidiMessage(uint32_t message) {
	if (!instance_synth) {
		return;
	}

	int event = message & 0xFF;
	int channel = event & 0x0F;
	int param1 = (message >> 8) & 0x7F;
	int param2 = (message >> 16) & 0x7F;

	switch (event & 0xF0){
		case 0x80:
			fluid_synth_noteoff(instance_synth, channel, param1);
			break;
		case 0x90:
			fluid_synth_noteon(instance_synth, channel, param1, param2);
			break;
		case 0xA0:
			fluid_synth_key_pressure(instance_synth, event, param1, param2);
			break;
		case 0xB0:
			fluid_synth_cc(instance_synth, channel, param1, param2);
			break;
		case 0xC0:
			fluid_synth_program_change(instance_synth, channel, param1);
			break;
		case 0xD0:
			fluid_synth_channel_pressure(instance_synth, channel, param1);
			break;
		case 0xE0:
			fluid_synth_pitch_bend(instance_synth, channel, ((param2 & 0x7F) << 7) | (param1 & 0x7F));
			break;
		case 0xFF:
			fluid_synth_system_reset(instance_synth);
			break;
		default:
			break;
	}
}

void FluidSynthDecoder::OnMidiReset() {
	if (!instance_synth) {
		return;
	}

	fluid_synth_system_reset(instance_synth);
}

#endif
