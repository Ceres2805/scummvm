/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/debug.h"
#include "common/file.h"
#include "common/mutex.h"
#include "common/textconsole.h"
#include "common/queue.h"
#include "common/util.h"

#include "audio/audiostream.h"
#include "audio/decoders/flac.h"
#include "audio/decoders/mp3.h"
#include "audio/decoders/quicktime.h"
#include "audio/decoders/raw.h"
#include "audio/decoders/vorbis.h"
#include "audio/decoders/wave.h"
#include "audio/mixer.h"


namespace Audio {

struct StreamFileFormat {
	/** Decodername */
	const char *decoderName;
	const char *fileExtension;
	/**
	 * Pointer to a function which tries to open a file of type StreamFormat.
	 * Return NULL in case of an error (invalid/nonexisting file).
	 */
	SeekableAudioStream *(*openStreamFile)(Common::SeekableReadStream *stream, DisposeAfterUse::Flag disposeAfterUse);
};

static const StreamFileFormat STREAM_FILEFORMATS[] = {
	/* decoderName,  fileExt, openStreamFunction */
#ifdef USE_FLAC
	{ "FLAC",         ".flac", makeFLACStream },
	{ "FLAC",         ".fla",  makeFLACStream },
#endif
#ifdef USE_VORBIS
	{ "Ogg Vorbis",   ".ogg",  makeVorbisStream },
#endif
#ifdef USE_MAD
	{ "MPEG Layer 3", ".mp3",  makeMP3Stream },
#endif
	{ "MPEG-4 Audio", ".m4a",  makeQuickTimeStream },
	{ "WAV",          ".wav",  makeWAVStream },
};

SeekableAudioStream *SeekableAudioStream::openStreamFile(const Common::Path &basename) {
	SeekableAudioStream *stream = nullptr;
	Common::File *fileHandle = new Common::File();

	for (int i = 0; i < ARRAYSIZE(STREAM_FILEFORMATS); ++i) {
		Common::Path filename = basename.append(STREAM_FILEFORMATS[i].fileExtension);
		fileHandle->open(filename);
		if (fileHandle->isOpen()) {
			// Create the stream object
			stream = STREAM_FILEFORMATS[i].openStreamFile(fileHandle, DisposeAfterUse::YES);
			fileHandle = nullptr;
			break;
		}
	}

	delete fileHandle;

	if (stream == nullptr)
		debug(1, "SeekableAudioStream::openStreamFile: Could not open compressed AudioFile %s", basename.toString(Common::Path::kNativeSeparator).c_str());

	return stream;
}

#pragma mark -
#pragma mark --- LoopingAudioStream ---
#pragma mark -

LoopingAudioStream::LoopingAudioStream(Common::DisposablePtr<RewindableAudioStream>&& stream, uint loops, bool rewind)
        : _parent(Common::move(stream)), _loops(loops), _completeIterations(0) {
	assert(_parent);

	if (rewind && !_parent->rewind()) {
		error("LoopingAudioStream::LoopingAudioStream: stream could not be rewound");
		_loops = _completeIterations = 1;
	}
	if (_parent->endOfStream()) {
		// Apparently this is an empty stream
		_loops = _completeIterations = 1;
	}
}

LoopingAudioStream::LoopingAudioStream(RewindableAudioStream *stream, uint loops, DisposeAfterUse::Flag disposeAfterUse, bool rewind)
	: LoopingAudioStream(Common::move(Common::DisposablePtr<RewindableAudioStream>(stream, disposeAfterUse)), loops, rewind) {}

int LoopingAudioStream::readBuffer(int16 *buffer, const int numSamples) {
	if ((_loops && _completeIterations == _loops) || !numSamples)
		return 0;

	int samplesRead = _parent->readBuffer(buffer, numSamples);

	if (_parent->endOfStream()) {
		++_completeIterations;
		if (_completeIterations == _loops)
			return samplesRead;

		const int remainingSamples = numSamples - samplesRead;

		if (!_parent->rewind()) {
			error("LoopingAudioStream::readBuffer: stream could not be rewound");
			_loops = _completeIterations;
			return samplesRead;
		}
		if (_parent->endOfStream()) {
			// Apparently this is an empty stream
			_loops = _completeIterations;
		}

		return samplesRead + readBuffer(buffer + samplesRead, remainingSamples);
	}

	return samplesRead;
}

bool LoopingAudioStream::endOfData() const {
	return (_loops != 0 && _completeIterations == _loops) || _parent->endOfData();
}

bool LoopingAudioStream::endOfStream() const {
	return _loops != 0 && _completeIterations == _loops;
}

AudioStream *makeLoopingAudioStream(RewindableAudioStream *stream, uint loops) {
	if (loops != 1)
		return new LoopingAudioStream(stream, loops);
	else
		return stream;
}

AudioStream *makeLoopingAudioStream(SeekableAudioStream *stream, Timestamp start, Timestamp end, uint loops) {
	if (!start.totalNumberOfFrames() && (!end.totalNumberOfFrames() || end == stream->getLength())) {
		return makeLoopingAudioStream(stream, loops);
	} else {
		if (!end.totalNumberOfFrames())
			end = stream->getLength();

		if (start >= end) {
			warning("makeLoopingAudioStream: start (%d) >= end (%d)", start.msecs(), end.msecs());
			delete stream;
			return nullptr;
		}

		return makeLoopingAudioStream(new SubSeekableAudioStream(stream, start, end), loops);
	}
}

#pragma mark -
#pragma mark --- SubLoopingAudioStream ---
#pragma mark -

SubLoopingAudioStream::SubLoopingAudioStream(SeekableAudioStream *stream,
											 uint loops,
											 const Timestamp loopStart,
											 const Timestamp loopEnd,
											 DisposeAfterUse::Flag disposeAfterUse)
	: _parent(stream, disposeAfterUse), _loops(loops), _completeIterations(0),
	  _pos(0, getRate() * (isStereo() ? 2 : 1)),
	  _loopStart(convertTimeToStreamPos(loopStart, getRate(), isStereo())),
	  _loopEnd(convertTimeToStreamPos(loopEnd, getRate(), isStereo())) {
	assert(loopStart < loopEnd);
	assert(stream);

	if (!_parent->rewind())
		_loops = _completeIterations = 1;
}

int SubLoopingAudioStream::readBuffer(int16 *buffer, const int numSamples) {
	if ((_loops && _completeIterations == _loops) || !numSamples)
		return 0;

	int framesLeft = MIN(_loopEnd.frameDiff(_pos), numSamples);
	int framesRead = _parent->readBuffer(buffer, framesLeft);
	_pos = _pos.addFrames(framesRead);

	if (framesRead < framesLeft && _parent->endOfStream()) {
		error("SubLoopingAudioStream::readBuffer: Parent stream ended prematurely");
		if (!_completeIterations)
			_completeIterations = 1;
		_loops = _completeIterations;
		return framesRead;
	} else if (_pos == _loopEnd) {
		++_completeIterations;
		if (_completeIterations == _loops)
			return framesRead;

		if (!_parent->seek(_loopStart)) {
			error("SubLoopingAudioStream::readBuffer: Failed to seek to loop start");
			_loops = _completeIterations;
			return framesRead;
		}

		_pos = _loopStart;
		framesLeft = numSamples - framesLeft;
		return framesRead + readBuffer(buffer + framesRead, framesLeft);
	} else {
		return framesRead;
	}
}

bool SubLoopingAudioStream::endOfData() const {
	// We're out of data if this stream is finished or the parent
	// has run out of data for now.
	return (_loops != 0 && _completeIterations == _loops) || _parent->endOfData();
}

bool SubLoopingAudioStream::endOfStream() const {
	// The end of the stream has been reached only when we've gone
	// through all the iterations.
	return _loops != 0 && _completeIterations == _loops;
}

#pragma mark -
#pragma mark --- SubSeekableAudioStream ---
#pragma mark -

SubSeekableAudioStream::SubSeekableAudioStream(SeekableAudioStream *parent, const Timestamp start, const Timestamp end, DisposeAfterUse::Flag disposeAfterUse)
	: _parent(parent, disposeAfterUse),
	  _start(convertTimeToStreamPos(start, getRate(), isStereo())),
	  _pos(0, getRate() * (isStereo() ? 2 : 1)),
	  _length(convertTimeToStreamPos(end, getRate(), isStereo()) - _start) {

	assert(_length.totalNumberOfFrames() % (isStereo() ? 2 : 1) == 0);
	_parent->seek(_start);
}

int SubSeekableAudioStream::readBuffer(int16 *buffer, const int numSamples) {
	int framesLeft = MIN(_length.frameDiff(_pos), numSamples);
	int framesRead = _parent->readBuffer(buffer, framesLeft);
	_pos = _pos.addFrames(framesRead);
	return framesRead;
}

bool SubSeekableAudioStream::seek(const Timestamp &where) {
	_pos = convertTimeToStreamPos(where, getRate(), isStereo());
	if (_pos > _length) {
		_pos = _length;
		return false;
	}

	if (_parent->seek(_pos + _start)) {
		return true;
	} else {
		_pos = _length;
		return false;
	}
}

#pragma mark -
#pragma mark --- Queueing audio stream ---
#pragma mark -


void QueuingAudioStream::queueBuffer(byte *data, uint32 size, DisposeAfterUse::Flag disposeAfterUse, byte flags) {
	AudioStream *stream = makeRawStream(data, size, getRate(), flags, disposeAfterUse);
	queueAudioStream(stream, DisposeAfterUse::YES);
}


class QueuingAudioStreamImpl : public QueuingAudioStream {
private:
	/**
	 * We queue a number of (pointers to) audio stream objects.
	 * In addition, we need to remember for each stream whether
	 * to dispose it after all data has been read from it.
	 * Hence, we don't store pointers to stream objects directly,
	 * but rather StreamHolder structs.
	 */
	struct StreamHolder {
		AudioStream *_stream;
		DisposeAfterUse::Flag _disposeAfterUse;
		StreamHolder(AudioStream *stream, DisposeAfterUse::Flag disposeAfterUse)
		    : _stream(stream),
		      _disposeAfterUse(disposeAfterUse) {}
	};

	/**
	 * The sampling rate of this audio stream.
	 */
	const int _rate;

	/**
	 * Whether this audio stream is mono (=false) or stereo (=true).
	 */
	const int _stereo;

	/**
	 * This flag is set by the finish() method only. See there for more details.
	 */
	bool _finished;

	/**
	 * A mutex to avoid access problems (causing e.g. corruption of
	 * the linked list) in thread aware environments.
	 */
	Common::Mutex _mutex;

	/**
	 * The queue of audio streams.
	 */
	Common::Queue<StreamHolder> _queue;

public:
	QueuingAudioStreamImpl(int rate, bool stereo)
	    : _rate(rate), _stereo(stereo), _finished(false) {}
	~QueuingAudioStreamImpl();

	// Implement the AudioStream API
	int readBuffer(int16 *buffer, const int numSamples) override;
	bool isStereo() const override { return _stereo; }
	int getRate() const override { return _rate; }

	bool endOfData() const override {
		Common::StackLock lock(_mutex);
		return _queue.empty() || _queue.front()._stream->endOfData();
	}

	bool endOfStream() const override {
		Common::StackLock lock(_mutex);
		return _finished && _queue.empty();
	}

	// Implement the QueuingAudioStream API
	void queueAudioStream(AudioStream *stream, DisposeAfterUse::Flag disposeAfterUse) override;

	void finish() override {
		Common::StackLock lock(_mutex);
		_finished = true;
	}

	uint32 numQueuedStreams() const override {
		Common::StackLock lock(_mutex);
		return _queue.size();
	}
};

QueuingAudioStreamImpl::~QueuingAudioStreamImpl() {
	while (!_queue.empty()) {
		StreamHolder tmp = _queue.pop();
		if (tmp._disposeAfterUse == DisposeAfterUse::YES)
			delete tmp._stream;
	}
}

void QueuingAudioStreamImpl::queueAudioStream(AudioStream *stream, DisposeAfterUse::Flag disposeAfterUse) {
	assert(!_finished);
	if ((stream->getRate() != getRate()) || (stream->isStereo() != isStereo()))
		error("QueuingAudioStreamImpl::queueAudioStream: stream has mismatched parameters");

	Common::StackLock lock(_mutex);
	_queue.push(StreamHolder(stream, disposeAfterUse));
}

int QueuingAudioStreamImpl::readBuffer(int16 *buffer, const int numSamples) {
	Common::StackLock lock(_mutex);
	int samplesDecoded = 0;

	while (samplesDecoded < numSamples && !_queue.empty()) {
		AudioStream *stream = _queue.front()._stream;
		samplesDecoded += stream->readBuffer(buffer + samplesDecoded, numSamples - samplesDecoded);

		// Done with the stream completely
		if (stream->endOfStream()) {
			StreamHolder tmp = _queue.pop();
			if (tmp._disposeAfterUse == DisposeAfterUse::YES)
				delete stream;
			continue;
		}

		// Done with data but not the stream, bail out
		if (stream->endOfData())
			break;
	}

	return samplesDecoded;
}

QueuingAudioStream *makeQueuingAudioStream(int rate, bool stereo) {
	return new QueuingAudioStreamImpl(rate, stereo);
}

Timestamp convertTimeToStreamPos(const Timestamp &where, int rate, bool isStereo) {
	Timestamp result(where.convertToFramerate(rate * (isStereo ? 2 : 1)));

	// When the Stream is a stereo stream, we have to assure
	// that the sample position is an even number.
	if (isStereo && (result.totalNumberOfFrames() & 1))
		result = result.addFrames(-1); // We cut off one sample here.

	// Since Timestamp allows sub-frame-precision it might lead to odd behaviors
	// when we would just return result.
	//
	// An example is when converting the timestamp 500ms to a 11025 Hz based
	// stream. It would have an internal frame counter of 5512.5. Now when
	// doing calculations at frame precision, this might lead to unexpected
	// results: The frame difference between a timestamp 1000ms and the above
	// mentioned timestamp (both with 11025 as framerate) would be 5512,
	// instead of 5513, which is what a frame-precision based code would expect.
	//
	// By creating a new Timestamp with the given parameters, we create a
	// Timestamp with frame-precision, which just drops a sub-frame-precision
	// information (i.e. rounds down).
	return Timestamp(result.secs(), result.numberOfFrames(), result.framerate());
}

/**
 * An AudioStream wrapper that cuts off the amount of samples read after a
 * given time length is reached.
 */
class LimitingAudioStream : public AudioStream {
public:
	LimitingAudioStream(AudioStream *parentStream, const Audio::Timestamp &length, DisposeAfterUse::Flag disposeAfterUse) :
			_parentStream(parentStream), _samplesRead(0), _disposeAfterUse(disposeAfterUse),
			_totalSamples(length.convertToFramerate(getRate()).totalNumberOfFrames() * getChannels()) {}

	~LimitingAudioStream() {
		if (_disposeAfterUse == DisposeAfterUse::YES)
			delete _parentStream;
	}

	int readBuffer(int16 *buffer, const int numSamples) override {
		// Cap us off so we don't read past _totalSamples
		int samplesRead = _parentStream->readBuffer(buffer, MIN<int>(numSamples, _totalSamples - _samplesRead));
		_samplesRead += samplesRead;
		return samplesRead;
	}

	bool endOfData() const override { return _parentStream->endOfData() || reachedLimit(); }
	bool endOfStream() const override { return _parentStream->endOfStream() || reachedLimit(); }
	bool isStereo() const override { return _parentStream->isStereo(); }
	int getRate() const override { return _parentStream->getRate(); }

private:
	int getChannels() const { return isStereo() ? 2 : 1; }
	bool reachedLimit() const { return _samplesRead >= _totalSamples; }

	AudioStream *_parentStream;
	DisposeAfterUse::Flag _disposeAfterUse;
	uint32 _totalSamples, _samplesRead;
};

AudioStream *makeLimitingAudioStream(AudioStream *parentStream, const Timestamp &length, DisposeAfterUse::Flag disposeAfterUse) {
	return new LimitingAudioStream(parentStream, length, disposeAfterUse);
}

/**
 * An AudioStream that plays nothing and immediately returns that
 * the endOfStream() has been reached
 */
class NullAudioStream : public AudioStream {
public:
	bool isStereo() const override { return false; }
	int getRate() const override;
	int readBuffer(int16 *data, const int numSamples) override { return 0; }
	bool endOfData() const override { return true; }
};

int NullAudioStream::getRate() const {
	return g_system->getMixer()->getOutputRate();
}

AudioStream *makeNullAudioStream() {
	return new NullAudioStream();
}

/**
 * An AudioStream that just returns silent samples and runs infinitely.
 */
class SilentAudioStream : public AudioStream {
public:
	SilentAudioStream(int rate, bool stereo) : _rate(rate), _isStereo(stereo) {}

	int readBuffer(int16 *buffer, const int numSamples) override {
		memset(buffer, 0, numSamples * 2);
		return numSamples;
	}

	bool endOfData() const override { return false; } // it never ends!
	bool isStereo() const override { return _isStereo; }
	int getRate() const override { return _rate; }

private:
	int _rate;
	bool _isStereo;
};

AudioStream *makeSilentAudioStream(int rate, bool stereo) {
	return new SilentAudioStream(rate, stereo);
}

} // End of namespace Audio
