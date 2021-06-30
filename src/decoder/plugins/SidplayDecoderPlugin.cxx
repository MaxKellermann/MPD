/*
 * Copyright 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "SidplayDecoderPlugin.hxx"
#include "decoder/Features.h"
#include "../DecoderAPI.hxx"
#include "tag/Handler.hxx"
#include "tag/Builder.hxx"
#include "song/DetachedSong.hxx"
#include "fs/NarrowPath.hxx"
#include "fs/Path.hxx"
#include "fs/AllocatedPath.hxx"
#include "lib/icu/Converter.hxx"
#ifdef HAVE_SIDPLAYFP
#include "fs/io/FileReader.hxx"
#include "util/RuntimeError.hxx"
#endif
#include "util/StringFormat.hxx"
#include "util/StringView.hxx"
#include "util/Domain.hxx"
#include "util/AllocatedString.hxx"
#include "util/CharUtil.hxx"
#include "util/ByteOrder.hxx"
#include "util/RuntimeError.hxx"
#include "Log.hxx"

#ifdef HAVE_SIDPLAYFP
#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidInfo.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/builders/resid.h>
#include <sidplayfp/builders/residfp.h>
#include <sidplayfp/SidDatabase.h>
#else
#include <sidplay/sidplay2.h>
#include <sidplay/builders/resid.h>
#include <sidplay/utils/SidTuneMod.h>
#include <sidplay/utils/SidDatabase.h>
#endif

#include <iterator>
#include <memory>

#include <string.h>

#define SUBTUNE_PREFIX "tune_"

static constexpr Domain sidplay_domain("sidplay");

struct SidplayGlobal {
	std::unique_ptr<SidDatabase> songlength_database;

	bool all_files_are_containers;
	unsigned default_songlength;
	std::string default_genre;

	bool filter_setting;

#ifdef HAVE_SIDPLAYFP
	std::unique_ptr<uint8_t[]> kernal, basic;
#endif

	explicit SidplayGlobal(const ConfigBlock &block);
};

static SidplayGlobal *sidplay_global;

#ifdef HAVE_SIDPLAYFP
static constexpr unsigned rom_size = 8192;

static void loadRom(const Path rom_path, uint8_t *dump)
{
	FileReader romDump(rom_path);
	if (romDump.Read(dump, rom_size) != rom_size)
	{
		throw FormatRuntimeError
			("Could not load rom dump '%s'", rom_path.c_str());
	}
}
#endif

/**
 * Throws on error.
 */
static std::unique_ptr<SidDatabase>
sidplay_load_songlength_db(const Path path)
{
	auto db = std::make_unique<SidDatabase>();
#ifdef HAVE_SIDPLAYFP
	bool error = !db->open(NarrowPath(path));
#else
	bool error = db->open(NarrowPath(path)) < 0;
#endif
	if (error)
		throw FormatRuntimeError("unable to read songlengths file %s: %s",
					 path.c_str(), db->error());

	return db;
}

inline
SidplayGlobal::SidplayGlobal(const ConfigBlock &block)
{
	/* read the songlengths database file */
	const auto database_path = block.GetPath("songlength_database");
	if (!database_path.IsNull())
		songlength_database = sidplay_load_songlength_db(database_path);

	default_songlength = block.GetPositiveValue("default_songlength", 0U);

	default_genre = block.GetBlockValue("default_genre", "");

	all_files_are_containers =
		block.GetBlockValue("all_files_are_containers", true);

	filter_setting = block.GetBlockValue("filter", true);

#ifdef HAVE_SIDPLAYFP
	/* read kernal rom dump file */
	const auto kernal_path = block.GetPath("kernal");
	if (!kernal_path.IsNull())
	{
		kernal.reset(new uint8_t[rom_size]);
		loadRom(kernal_path, kernal.get());
	}

	/* read basic rom dump file */
	const auto basic_path = block.GetPath("basic");
	if (!basic_path.IsNull())
	{
		basic.reset(new uint8_t[rom_size]);
		loadRom(basic_path, basic.get());
	}
#endif
}

static bool
sidplay_init(const ConfigBlock &block)
{
	sidplay_global = new SidplayGlobal(block);
	return true;
}

static void
sidplay_finish() noexcept
{
	delete sidplay_global;
}

struct SidplayContainerPath {
	AllocatedPath path;
	unsigned track;
};

gcc_pure
static unsigned
ParseSubtuneName(const char *base) noexcept
{
	if (memcmp(base, SUBTUNE_PREFIX, sizeof(SUBTUNE_PREFIX) - 1) != 0)
		return 0;

	base += sizeof(SUBTUNE_PREFIX) - 1;

	char *endptr;
	auto track = strtoul(base, &endptr, 10);
	if (endptr == base || *endptr != '.')
		return 0;

	return track;
}

/**
 * returns the file path stripped of any /tune_xxx.* subtune suffix
 * and the track number (or 1 if no "tune_xxx" suffix is present).
 */
static SidplayContainerPath
ParseContainerPath(Path path_fs) noexcept
{
	const Path base = path_fs.GetBase();
	unsigned track;
	if (base.IsNull() ||
	    (track = ParseSubtuneName(NarrowPath(base))) < 1)
		return { AllocatedPath(path_fs), 1 };

	return { path_fs.GetDirectoryName(), track };
}

/**
 * This is a template, because libsidplay requires SidTuneMod while
 * libsidplayfp requires just a plain Sidtune.
 */
template<typename T>
static SignedSongTime
get_song_length(T &tune) noexcept
{
	assert(tune.getStatus());

	if (sidplay_global->songlength_database == nullptr)
		return SignedSongTime::Negative();

#if LIBSIDPLAYFP_VERSION_MAJ >= 2
	const auto lengthms =
		sidplay_global->songlength_database->lengthMs(tune);
	/* check for new song length format since HVSC#68 or later */
	if (lengthms < 0)
	{
#endif
		/* old song lenghth format */
		const auto length =
			sidplay_global->songlength_database->length(tune);
		if (length >= 0)
			return SignedSongTime::FromS(length);
		return SignedSongTime::Negative();

#if LIBSIDPLAYFP_VERSION_MAJ >= 2
	}
	return SignedSongTime::FromMS(lengthms);
#endif
}

static void
sidplay_file_decode(DecoderClient &client, Path path_fs)
{
	int channels;

	/* load the tune */

	const auto container = ParseContainerPath(path_fs);
#ifdef HAVE_SIDPLAYFP
	SidTune tune(NarrowPath(container.path));
#else
	SidTuneMod tune(NarrowPath(container.path));
#endif
	if (!tune.getStatus()) {
#ifdef HAVE_SIDPLAYFP
		const char *error = tune.statusString();
#else
		const char *error = tune.getInfo().statusString;
#endif
		FmtWarning(sidplay_domain, "failed to load file: {}", error);
		return;
	}

	const int song_num = container.track;
	tune.selectSong(song_num);

	auto duration = get_song_length(tune);
	if (duration.IsNegative() && sidplay_global->default_songlength > 0)
		duration = SongTime::FromS(sidplay_global->default_songlength);

	/* initialize the player */

#ifdef HAVE_SIDPLAYFP
	sidplayfp player;

	player.setRoms(sidplay_global->kernal.get(),
		       sidplay_global->basic.get(),
		       nullptr);
#else
	sidplay2 player;
#endif
#ifdef HAVE_SIDPLAYFP
	bool error = !player.load(&tune);
#else
	bool error = player.load(&tune) < 0;
#endif
	if (error) {
		FmtWarning(sidplay_domain,
			   "sidplay2.load() failed: {}", player.error());
		return;
	}

	/* initialize the builder */

#ifdef HAVE_SIDPLAYFP
	ReSIDfpBuilder builder("ReSID");
	if (!builder.getStatus()) {
		FmtWarning(sidplay_domain,
			   "failed to initialize ReSIDfpBuilder: {}",
			   builder.error());
		return;
	}

	builder.create(player.info().maxsids());
	if (!builder.getStatus()) {
		FmtWarning(sidplay_domain,
			   "ReSIDfpBuilder.create() failed: {}",
			   builder.error());
		return;
	}
#else
	ReSIDBuilder builder("ReSID");
	builder.create(player.info().maxsids);
	if (!builder) {
		FmtWarning(sidplay_domain, "ReSIDBuilder.create() failed: {}",
			   builder.error());
		return;
	}
#endif

	builder.filter(sidplay_global->filter_setting);
#ifdef HAVE_SIDPLAYFP
	if (!builder.getStatus()) {
		FmtWarning(sidplay_domain,
			   "ReSIDfpBuilder.filter() failed: {}",
			   builder.error());
		return;
	}
#else
	if (!builder) {
		FmtWarning(sidplay_domain, "ReSIDBuilder.filter() failed: {}",
			   builder.error());
		return;
	}
#endif

	/* configure the player */

	auto config = player.config();

#ifndef HAVE_SIDPLAYFP
	config.clockDefault = SID2_CLOCK_PAL;
	config.clockForced = true;
	config.clockSpeed = SID2_CLOCK_CORRECT;
#endif
	config.frequency = 48000;
#ifndef HAVE_SIDPLAYFP
	config.optimisation = SID2_DEFAULT_OPTIMISATION;

	config.precision = 16;
	config.sidDefault = SID2_MOS6581;
#endif
	config.sidEmulation = &builder;
#ifdef HAVE_SIDPLAYFP
	config.samplingMethod = SidConfig::INTERPOLATE;
	config.fastSampling = false;
#else
	config.sidModel = SID2_MODEL_CORRECT;
	config.sidSamples = true;
	config.sampleFormat = IsLittleEndian()
		? SID2_LITTLE_SIGNED
		: SID2_BIG_SIGNED;
#endif

#ifdef HAVE_SIDPLAYFP
	const bool stereo = tune.getInfo()->sidChips() >= 2;
#else
	const bool stereo = tune.isStereo();
#endif

	if (stereo) {
#ifdef HAVE_SIDPLAYFP
		config.playback = SidConfig::STEREO;
#else
		config.playback = sid2_stereo;
#endif
		channels = 2;
	} else {
#ifdef HAVE_SIDPLAYFP
		config.playback = SidConfig::MONO;
#else
		config.playback = sid2_mono;
#endif
		channels = 1;
	}

#ifdef HAVE_SIDPLAYFP
	error = !player.config(config);
#else
	error = player.config(config) < 0;
#endif
	if (error) {
		FmtWarning(sidplay_domain,
			   "sidplay2.config() failed: {}", player.error());
		return;
	}

	/* initialize the MPD decoder */

	const AudioFormat audio_format(48000, SampleFormat::S16, channels);
	assert(audio_format.IsValid());

	client.Ready(audio_format, true, duration);

	/* .. and play */

#ifdef HAVE_SIDPLAYFP
	constexpr unsigned timebase = 1;
#else
	const unsigned timebase = player.timebase();
#endif
	const unsigned end = duration.IsNegative()
		? 0U
		: duration.ToScale<uint64_t>(timebase);

	DecoderCommand cmd;
	do {
		short buffer[4096];

		const auto result = player.play(buffer, std::size(buffer));
		if (result <= 0)
			break;

#ifdef HAVE_SIDPLAYFP
		/* libsidplayfp returns the number of samples */
		const size_t nbytes = result * sizeof(buffer[0]);
#else
		/* libsidplay2 returns the number of bytes */
		const size_t nbytes = result;
#endif

		client.SubmitTimestamp(FloatDuration(player.time()) / timebase);

		cmd = client.SubmitData(nullptr, buffer, nbytes, 0);

		if (cmd == DecoderCommand::SEEK) {
			unsigned data_time = player.time();
			unsigned target_time =
				client.GetSeekTime().ToScale(timebase);

			/* can't rewind so return to zero and seek forward */
			if(target_time<data_time) {
				player.stop();
				data_time=0;
			}

			/* ignore data until target time is reached */
			while (data_time < target_time &&
			       player.play(buffer, std::size(buffer)) > 0)
				data_time = player.time();

			client.CommandFinished();
		}

		if (end > 0 && player.time() >= end)
			break;

	} while (cmd != DecoderCommand::STOP);
}

static AllocatedString
Windows1252ToUTF8(const char *s) noexcept
{
#ifdef HAVE_ICU_CONVERTER
	try {
		return IcuConverter::Create("windows-1252")->ToUTF8(s);
	} catch (...) { }
#endif

	/*
	 * Fallback to not transcoding windows-1252 to utf-8, that may result
	 * in invalid utf-8 unless nonprintable characters are replaced.
	 */
	AllocatedString t(s);

	for (size_t i = 0; t[i] != AllocatedString::SENTINEL; i++)
		if (!IsPrintableASCII(t[i]))
			t[i] = '?';

	return t;
}

gcc_pure
static AllocatedString
GetInfoString(const SidTuneInfo &info, unsigned i) noexcept
{
#ifdef HAVE_SIDPLAYFP
	const char *s = info.numberOfInfoStrings() > i
		? info.infoString(i)
		: "";
#else
	const char *s = info.numberOfInfoStrings > i
		? info.infoString[i]
		: "";
#endif

	return Windows1252ToUTF8(s);
}

gcc_pure
static AllocatedString
GetDateString(const SidTuneInfo &info) noexcept
{
	/*
	 * Field 2 is called <released>, previously used as <copyright>.
	 * It is formatted <year><space><company or author or group>,
	 * where <year> may be <YYYY>, <YYY?>, <YY??> or <YYYY-YY>, for
	 * example "1987", "199?", "19??" or "1985-87". The <company or
	 * author or group> may be for example Rob Hubbard. A full field
	 * may be for example "1987 Rob Hubbard".
	 */
	AllocatedString release = GetInfoString(info, 2);

	/* Keep the <year> part only for the date. */
	for (size_t i = 0; release[i] != AllocatedString::SENTINEL; i++)
		if (std::isspace(release[i])) {
			release[i] = AllocatedString::SENTINEL;
			break;
		}

	return release;
}

static void
ScanSidTuneInfo(const SidTuneInfo &info, unsigned track, unsigned n_tracks,
		TagHandler &handler) noexcept
{
	/* album */
	const auto album = GetInfoString(info, 0);

	handler.OnTag(TAG_ALBUM, album.c_str());

	if (n_tracks > 1) {
		const auto tag_title =
			StringFormat<1024>("%s (%u/%u)",
					   album.c_str(), track, n_tracks);
		handler.OnTag(TAG_TITLE, tag_title.c_str());
	} else
		handler.OnTag(TAG_TITLE, album.c_str());

	/* artist */
	const auto artist = GetInfoString(info, 1);
	if (!artist.empty())
		handler.OnTag(TAG_ARTIST, artist.c_str());

	/* genre */
	if (!sidplay_global->default_genre.empty())
		handler.OnTag(TAG_GENRE,
			      sidplay_global->default_genre.c_str());

	/* date */
	const auto date = GetDateString(info);
	if (!date.empty())
		handler.OnTag(TAG_DATE, date.c_str());

	/* track */
	handler.OnTag(TAG_TRACK, StringFormat<16>("%u", track).c_str());
}

static bool
sidplay_scan_file(Path path_fs, TagHandler &handler) noexcept
{
	const auto container = ParseContainerPath(path_fs);
	const unsigned song_num = container.track;

#ifdef HAVE_SIDPLAYFP
	SidTune tune(NarrowPath(container.path));
#else
	SidTuneMod tune(NarrowPath(container.path));
#endif
	if (!tune.getStatus())
		return false;

	tune.selectSong(song_num);

#ifdef HAVE_SIDPLAYFP
	const SidTuneInfo &info = *tune.getInfo();
	const unsigned n_tracks = info.songs();
#else
	const SidTuneInfo &info = tune.getInfo();
	const unsigned n_tracks = info.songs;
#endif

	ScanSidTuneInfo(info, song_num, n_tracks, handler);

	/* time */
	const auto duration = get_song_length(tune);
	if (!duration.IsNegative())
		handler.OnDuration(SongTime(duration));

	return true;
}

static std::forward_list<DetachedSong>
sidplay_container_scan(Path path_fs)
{
	std::forward_list<DetachedSong> list;

#ifdef HAVE_SIDPLAYFP
	SidTune tune((NarrowPath(path_fs)));
#else
	SidTuneMod tune((NarrrowPath(path_fs)));
#endif
	if (!tune.getStatus())
		return list;

#ifdef HAVE_SIDPLAYFP
	const SidTuneInfo &info = *tune.getInfo();
	const unsigned n_tracks = info.songs();
#else
	const SidTuneInfo &info = tune.getInfo();
	const unsigned n_tracks = info.songs;
#endif

	/* Don't treat sids containing a single tune
		as containers */
	if (!sidplay_global->all_files_are_containers && n_tracks < 2)
		return list;

	TagBuilder tag_builder;

	auto tail = list.before_begin();
	for (unsigned i = 1; i <= n_tracks; ++i) {
		tune.selectSong(i);

		AddTagHandler h(tag_builder);
		ScanSidTuneInfo(info, i, n_tracks, h);

		const SignedSongTime duration = get_song_length(tune);
		if (!duration.IsNegative())
			h.OnDuration(SongTime(duration));

		/* Construct container/tune path names, eg.
		   Delta.sid/tune_001.sid */
		tail = list.emplace_after(tail,
					  StringFormat<32>(SUBTUNE_PREFIX "%03u.sid", i),
					  tag_builder.Commit());
	}

	return list;
}

static const char *const sidplay_suffixes[] = {
	"sid",
	"mus",
	"str",
	"prg",
	"P00",
	nullptr
};

constexpr DecoderPlugin sidplay_decoder_plugin =
	DecoderPlugin("sidplay", sidplay_file_decode, sidplay_scan_file)
	.WithInit(sidplay_init, sidplay_finish)
	.WithContainer(sidplay_container_scan)
	.WithSuffixes(sidplay_suffixes);
