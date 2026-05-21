/**************************************************************************/
/*  audio_stream_mp3.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#define DR_MP3_FLOAT_OUTPUT
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO

#include "audio_stream_mp3.h"
#include "core/io/file_access.h"

#include "thirdparty/dr_libs/dr_bridge.h"

int AudioStreamPlaybackMP3::_read_frames(AudioFrame *p_buffer, int p_frames) {
	if (p_frames <= 0) {
		return 0;
	}

	const uint32_t channels = MAX((uint32_t)1, (uint32_t)mp3d.channels);
	const int frames_to_read = MIN(p_frames, DECODE_CHUNK_FRAMES);
	const uint32_t sample_count = (uint32_t)frames_to_read * channels;
	ERR_FAIL_COND_V(decode_buffer.size() < sample_count, 0);

	const drmp3_uint64 frames_read = drmp3_read_pcm_frames_f32(&mp3d, frames_to_read, decode_buffer.ptr());
	for (drmp3_uint64 i = 0; i < frames_read; i++) {
		const float *frame = decode_buffer.ptr() + (i * channels);
		p_buffer[i] = AudioFrame(frame[0], frame[channels - 1]);
	}

	return (int)frames_read;
}

bool AudioStreamPlaybackMP3::_seek_internal(double p_time, uint64_t p_loop_frame_limit) {
	if (!active || mp3_stream->frame_count == 0) {
		frames_mixed = 0;
		return false;
	}

	if (!Math::is_finite(p_time) || p_time < 0.0) {
		p_time = 0.0;
	}

	const double seek_frame_d = double(mp3_stream->sample_rate) * p_time;
	uint64_t seek_frame = 0;
	if (Math::is_finite(seek_frame_d) && seek_frame_d >= 0.0 && seek_frame_d < double(mp3_stream->frame_count)) {
		seek_frame = (uint64_t)seek_frame_d;
	}
	if (seek_frame >= mp3_stream->frame_count) {
		seek_frame = 0;
	}
	if (p_loop_frame_limit > 0 && seek_frame >= p_loop_frame_limit) {
		seek_frame = 0;
	}

	const uint64_t previous_frames_mixed = frames_mixed;
	if (!drmp3_seek_to_pcm_frame(&mp3d, seek_frame)) {
		frames_mixed = previous_frames_mixed;
		return false;
	}

	frames_mixed = seek_frame;
	return true;
}

int AudioStreamPlaybackMP3::_mix_internal(AudioFrame *p_buffer, int p_frames) {
	if (!active) {
		return 0;
	}

	int todo = p_frames;
	int out_offset = 0;

	int frames_mixed_this_step = p_frames;

	uint64_t beat_length_frames = 0;
	bool use_loop = looping_override ? looping : mp3_stream->loop;
	bool retried_loop_without_output = false;

	bool beat_loop = use_loop && mp3_stream->get_bpm() > 0 && mp3_stream->get_beat_count() > 0;
	if (beat_loop) {
		const double beat_length_frame_count = double(mp3_stream->get_beat_count()) * double(mp3_stream->sample_rate) * 60.0 / mp3_stream->get_bpm();
		if (beat_length_frame_count < 1.0 || beat_length_frame_count >= double(UINT64_MAX)) {
			beat_loop = false;
		} else {
			beat_length_frames = (uint64_t)beat_length_frame_count;
		}
	}

	while (todo && active) {
		int frames_to_read = MIN(todo, DECODE_CHUNK_FRAMES);
		if (beat_loop) {
			if (frames_mixed < beat_length_frames) {
				const uint64_t frames_until_loop = beat_length_frames - frames_mixed;
				frames_to_read = MIN(frames_to_read, (int)MIN(frames_until_loop, (uint64_t)INT_MAX));
			} else {
				frames_to_read = 1;
			}
		}

		const int frames_read = _read_frames(p_buffer + out_offset, frames_to_read);

		if (frames_read > 0) {
			for (int i = 0; i < frames_read; i++) {
				AudioFrame &frame = p_buffer[out_offset + i];
				if (loop_fade_remaining < FADE_SIZE) {
					frame += loop_fade[loop_fade_remaining] * (float(FADE_SIZE - loop_fade_remaining) / float(FADE_SIZE));
					loop_fade_remaining++;
				}
			}
			todo -= frames_read;
			out_offset += frames_read;
			frames_mixed += frames_read;
			retried_loop_without_output = false;

			if (beat_loop && frames_mixed >= beat_length_frames) {
				const int fade_frames = _read_frames(loop_fade, FADE_SIZE);
				for (int i = fade_frames; i < FADE_SIZE; i++) {
					loop_fade[i] = AudioFrame(0, 0);
				}
				loop_fade_remaining = 0;
				if (_seek_internal(mp3_stream->loop_offset, beat_length_frames)) {
					loops++;
				} else {
					frames_mixed_this_step = out_offset;
					for (int i = out_offset; i < p_frames; i++) {
						p_buffer[i] = AudioFrame(0, 0);
					}
					active = false;
					todo = 0;
				}
			}
		}

		else {
			//EOF
			if (use_loop && !retried_loop_without_output && _seek_internal(mp3_stream->loop_offset, beat_loop ? beat_length_frames : 0)) {
				loops++;
				retried_loop_without_output = true;
			} else {
				frames_mixed_this_step = p_frames - todo;
				//fill remainder with silence
				for (int i = out_offset; i < p_frames; i++) {
					p_buffer[i] = AudioFrame(0, 0);
				}
				active = false;
				todo = 0;
			}
		}
	}
	return frames_mixed_this_step;
}

float AudioStreamPlaybackMP3::get_stream_sampling_rate() {
	return mp3_stream->sample_rate;
}

void AudioStreamPlaybackMP3::start(double p_from_pos) {
	active = true;
	loop_fade_remaining = FADE_SIZE;
	loops = 0;
	if (!_seek_internal(p_from_pos)) {
		active = false;
		return;
	}
	begin_resample();
}

void AudioStreamPlaybackMP3::stop() {
	active = false;
}

bool AudioStreamPlaybackMP3::is_playing() const {
	return active;
}

int AudioStreamPlaybackMP3::get_loop_count() const {
	return loops;
}

double AudioStreamPlaybackMP3::get_playback_position() const {
	return double(frames_mixed) / mp3_stream->sample_rate;
}

void AudioStreamPlaybackMP3::seek(double p_time) {
	if (!active) {
		return;
	}

	loop_fade_remaining = FADE_SIZE;
	if (!_seek_internal(p_time)) {
		active = false;
	}
}

void AudioStreamPlaybackMP3::tag_used_streams() {
	mp3_stream->tag_used(get_playback_position());
}

void AudioStreamPlaybackMP3::set_is_sample(bool p_is_sample) {
	_is_sample = p_is_sample;
}

bool AudioStreamPlaybackMP3::get_is_sample() const {
	return _is_sample;
}

Ref<AudioSamplePlayback> AudioStreamPlaybackMP3::get_sample_playback() const {
	return sample_playback;
}

void AudioStreamPlaybackMP3::set_sample_playback(const Ref<AudioSamplePlayback> &p_playback) {
	sample_playback = p_playback;
	if (sample_playback.is_valid()) {
		sample_playback->stream_playback = Ref<AudioStreamPlayback>(this);
	}
}

void AudioStreamPlaybackMP3::set_parameter(const StringName &p_name, const Variant &p_value) {
	if (p_name == SNAME("looping")) {
		if (p_value == Variant()) {
			looping_override = false;
			looping = false;
		} else {
			looping_override = true;
			looping = p_value;
		}
	}
}

Variant AudioStreamPlaybackMP3::get_parameter(const StringName &p_name) const {
	if (looping_override && p_name == SNAME("looping")) {
		return looping;
	}
	return Variant();
}

AudioStreamPlaybackMP3::~AudioStreamPlaybackMP3() {
	drmp3_uninit(&mp3d);
}

Ref<AudioStreamPlayback> AudioStreamMP3::instantiate_playback() {
	Ref<AudioStreamPlaybackMP3> mp3s;

	ERR_FAIL_COND_V_MSG(data.is_empty(), mp3s,
			"This AudioStreamMP3 does not have an audio file assigned "
			"to it. AudioStreamMP3 should not be created from the "
			"inspector or with `.new()`. Instead, load an audio file.");

	mp3s.instantiate();
	mp3s->mp3_stream = Ref<AudioStreamMP3>(this);

	int success = drmp3_init_memory(&mp3s->mp3d, data.ptr(), data_len, (drmp3_allocation_callbacks *)&dr_alloc_calls);

	ERR_FAIL_COND_V(!success, Ref<AudioStreamPlaybackMP3>());

	mp3s->frames_mixed = 0;
	mp3s->active = false;
	mp3s->loops = 0;
	mp3s->decode_buffer.resize_uninitialized(AudioStreamPlaybackMP3::DECODE_CHUNK_FRAMES * MAX((uint32_t)1, (uint32_t)mp3s->mp3d.channels));

	return mp3s;
}

String AudioStreamMP3::get_stream_name() const {
	return ""; //return stream_name;
}

void AudioStreamMP3::clear_data() {
	data.clear();
}

void AudioStreamMP3::set_data(const Vector<uint8_t> &p_data) {
	int src_data_len = p_data.size();

	drmp3 *mp3d = memnew(drmp3);
	int success = drmp3_init_memory(mp3d, p_data.ptr(), src_data_len, (drmp3_allocation_callbacks *)&dr_alloc_calls);
	if (!success || mp3d->sampleRate == 0) {
		memdelete(mp3d);
		ERR_FAIL_MSG("Failed to decode mp3 file. Make sure it is a valid mp3 audio file.");
	}

	channels = mp3d->channels;
	sample_rate = mp3d->sampleRate;
	frame_count = drmp3_get_pcm_frame_count(mp3d);
	length = float(frame_count) / (mp3d->sampleRate);

	drmp3_uninit(mp3d);
	memdelete(mp3d);

	data = p_data;
	data_len = src_data_len;
}

Vector<uint8_t> AudioStreamMP3::get_data() const {
	return Vector<uint8_t>(data);
}

void AudioStreamMP3::set_loop(bool p_enable) {
	loop = p_enable;
}

bool AudioStreamMP3::has_loop() const {
	return loop;
}

void AudioStreamMP3::set_loop_offset(double p_seconds) {
	if (!Math::is_finite(p_seconds) || p_seconds < 0.0) {
		p_seconds = 0.0;
	}
	loop_offset = p_seconds;
}

double AudioStreamMP3::get_loop_offset() const {
	return loop_offset;
}

double AudioStreamMP3::get_length() const {
	return length;
}

bool AudioStreamMP3::is_monophonic() const {
	return false;
}

void AudioStreamMP3::get_parameter_list(List<Parameter> *r_parameters) {
	r_parameters->push_back(Parameter(PropertyInfo(Variant::BOOL, "looping", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CHECKABLE), Variant()));
}

void AudioStreamMP3::set_bpm(double p_bpm) {
	ERR_FAIL_COND(p_bpm < 0);
	bpm = p_bpm;
	emit_changed();
}

double AudioStreamMP3::get_bpm() const {
	return bpm;
}

void AudioStreamMP3::set_beat_count(int p_beat_count) {
	ERR_FAIL_COND(p_beat_count < 0);
	beat_count = p_beat_count;
	emit_changed();
}

int AudioStreamMP3::get_beat_count() const {
	return beat_count;
}

void AudioStreamMP3::set_bar_beats(int p_bar_beats) {
	ERR_FAIL_COND(p_bar_beats < 0);
	bar_beats = p_bar_beats;
	emit_changed();
}

int AudioStreamMP3::get_bar_beats() const {
	return bar_beats;
}

Ref<AudioSample> AudioStreamMP3::generate_sample() const {
	Ref<AudioSample> sample;
	sample.instantiate();
	sample->stream = this;
	sample->loop_mode = loop
			? AudioSample::LoopMode::LOOP_FORWARD
			: AudioSample::LoopMode::LOOP_DISABLED;
	sample->loop_begin = loop_offset;
	sample->loop_end = 0;
	return sample;
}

Ref<AudioStreamMP3> AudioStreamMP3::load_from_buffer(const Vector<uint8_t> &p_stream_data) {
	Ref<AudioStreamMP3> mp3_stream;
	mp3_stream.instantiate();
	mp3_stream->set_data(p_stream_data);
	ERR_FAIL_COND_V_MSG(mp3_stream->get_data().is_empty(), Ref<AudioStreamMP3>(), "MP3 decoding failed. Check that your data is a valid MP3 audio stream.");
	return mp3_stream;
}

Ref<AudioStreamMP3> AudioStreamMP3::load_from_file(const String &p_path) {
	const Vector<uint8_t> stream_data = FileAccess::get_file_as_bytes(p_path);
	ERR_FAIL_COND_V_MSG(stream_data.is_empty(), Ref<AudioStreamMP3>(), vformat("Cannot open file '%s'.", p_path));
	return load_from_buffer(stream_data);
}

void AudioStreamMP3::_bind_methods() {
	ClassDB::bind_static_method("AudioStreamMP3", D_METHOD("load_from_buffer", "stream_data"), &AudioStreamMP3::load_from_buffer);
	ClassDB::bind_static_method("AudioStreamMP3", D_METHOD("load_from_file", "path"), &AudioStreamMP3::load_from_file);

	ClassDB::bind_method(D_METHOD("set_data", "data"), &AudioStreamMP3::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &AudioStreamMP3::get_data);

	ClassDB::bind_method(D_METHOD("set_loop", "enable"), &AudioStreamMP3::set_loop);
	ClassDB::bind_method(D_METHOD("has_loop"), &AudioStreamMP3::has_loop);

	ClassDB::bind_method(D_METHOD("set_loop_offset", "seconds"), &AudioStreamMP3::set_loop_offset);
	ClassDB::bind_method(D_METHOD("get_loop_offset"), &AudioStreamMP3::get_loop_offset);

	ClassDB::bind_method(D_METHOD("set_bpm", "bpm"), &AudioStreamMP3::set_bpm);
	ClassDB::bind_method(D_METHOD("get_bpm"), &AudioStreamMP3::get_bpm);

	ClassDB::bind_method(D_METHOD("set_beat_count", "count"), &AudioStreamMP3::set_beat_count);
	ClassDB::bind_method(D_METHOD("get_beat_count"), &AudioStreamMP3::get_beat_count);

	ClassDB::bind_method(D_METHOD("set_bar_beats", "count"), &AudioStreamMP3::set_bar_beats);
	ClassDB::bind_method(D_METHOD("get_bar_beats"), &AudioStreamMP3::get_bar_beats);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bpm", PROPERTY_HINT_RANGE, "0,400,0.01,or_greater"), "set_bpm", "get_bpm");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "beat_count", PROPERTY_HINT_RANGE, "0,512,1,or_greater"), "set_beat_count", "get_beat_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bar_beats", PROPERTY_HINT_RANGE, "2,32,1,or_greater"), "set_bar_beats", "get_bar_beats");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "loop"), "set_loop", "has_loop");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "loop_offset"), "set_loop_offset", "get_loop_offset");
}

AudioStreamMP3::AudioStreamMP3() {
}

AudioStreamMP3::~AudioStreamMP3() {
	clear_data();
}
