/*
 * SSG VideoPlayer
 *  Multi process video player for windows.
 *
 * Copyright (c) 2010-2015 Safer Society Group Sweden AB
 * All Rights Reserved.
 *
 * This file is part of SSG VideoPlayer.
 *
 * SSG VideoPlayer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * SSG VideoPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SSG VideoPlayer.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <queue>
#include <functional>
#include <algorithm>
#include <SDL.h>

#include "Flog.h"
#include "AudioHandler.h"
#include "avlibs.h"
#include "TimeHandler.h"

struct Sample {
	int16_t chn[2];
	double ts = 0.0f;
	int frameIndex = 0;
};

class CAudioHandler : public AudioHandler
{
	IAudioDevicePtr device;
	AVCodecContext *aCodecCtx;
	AVCodec *aCodec;
	SwrContext* swr;
		
	uint8_t** dstBuf;
	int dstLineSize;
	int frameIndex = 0;
	bool skip = false;
	float volume = 1.0f;
	bool qvMute = false, mute = false;
	TimeHandlerPtr timeHandler;
	Sample lastSample;
	double audioFrameFrequency;
	
	std::queue<Sample> queue;
	
	double timeFromPts(uint64_t pts, AVRational timeBase){
		return (double)pts * av_q2d(timeBase);
	}

	public:
	CAudioHandler(AVCodecContext* aCodecCtx, IAudioDevicePtr audioDevice, TimeHandlerPtr timeHandler)
	{
		this->device = audioDevice;
		this->aCodecCtx = aCodecCtx;
		this->timeHandler = timeHandler;

		aCodec = avcodec_find_decoder(aCodecCtx->codec_id);

		if(!aCodec || avcodec_open2(aCodecCtx, aCodec, NULL) < 0)
		{
			FlogE("unsupported audio codec");
			return;
		}

		this->swr = 0;
		this->dstBuf = 0;
	}

	~CAudioHandler()
	{
		if(aCodecCtx){
			avcodec_close(aCodecCtx);
		}

		if(dstBuf)
			av_freep(&dstBuf[0]);
		av_freep(&dstBuf);

		if(this->swr)
			swr_free(&this->swr);
	}
	
	int getAudioQueueSize()
	{
		return (int)queue.size();
	}
	
	void clearQueue()
	{
		while(!queue.empty()){
			queue.pop();
		}
	}

	int fetchAudio(int16_t* data, int nSamples)
	{
		int fetched = 0;
		bool noSamples = queue.empty();
		Sample smp;
		double vt = 0.0;
		int freq = device->GetRate();

		vt = timeHandler->GetTime();

		// on a seek, audio might be far ahead of video,
		// so on a seek we skip audio until it matches
		// the current video time

		if(skip){
			while(!queue.empty())
			{
				if(queue.front().ts < vt){
					queue.pop();
				}else{
					skip = false;
					break;
				}
			}
		}

		float useVolume = (qvMute || mute) ? 0.0f : volume;

		for(int i = 0; i < nSamples; i++){
			if(queue.empty())
				break;

			Sample s = queue.front();
			queue.pop();

			data[i*2+0] = (int16_t)((float)s.chn[0] * useVolume);
			data[i*2+1] = (int16_t)((float)s.chn[1] * useVolume);

			smp = s;

			fetched++;
		}

		// keep track of the time between audio frames
		if(!noSamples && smp.frameIndex != lastSample.frameIndex){
			audioFrameFrequency = smp.ts - lastSample.ts;
		}

		double diff = smp.ts - vt;

		// add the difference between the last audio sample's timestamp and the current video time stamp
		// to the video time

		double addTime = std::max(0.0, diff);

		// if there are no queue (presumably the video has no audio track)
		// or, if the rate at which we get audio frames is too low 
		// (and the audio time is not too far ahead of the video time),
		// increase the video time by the audio rate instead

		if(noSamples || (smp.frameIndex == lastSample.frameIndex && vt < smp.ts + audioFrameFrequency)){
			addTime = 1.0 / (double)freq * (double)nSamples;
		}

		timeHandler->AddTime(addTime);

		if(!noSamples)
			lastSample = smp;

		return fetched;
	}

	int decode(AVPacket& packet, AVStream* stream, double timeWarp, bool addToQueue){
		if(!aCodec)
			return -1;

		AVFrame* frame = av_frame_alloc();
		FlogAssert(frame, "could not allocate frame");

		int got_frame = 0;
		int bytesDecoded = avcodec_decode_audio4(aCodecCtx, frame, &got_frame, &packet);

		if(bytesDecoded >= 0 && got_frame){
			int freq = device->GetRate();
			int channels = device->GetChannels();

			if(!this->swr){
				int64_t chLayout = frame->channel_layout != 0 ? frame->channel_layout : 
					av_get_default_channel_layout(frame->channels);

				this->swr = swr_alloc_set_opts(NULL, av_get_default_channel_layout(channels), 
					AV_SAMPLE_FMT_S16, freq, chLayout, (AVSampleFormat)frame->format, 
					frame->sample_rate, 0, NULL);

				FlogAssert(this->swr, "error allocating swr");
				
				swr_init(this->swr);

				int ret = av_samples_alloc_array_and_samples(&dstBuf, &dstLineSize, 2, 
					freq * channels, AV_SAMPLE_FMT_S16, 0);

				FlogAssert(ret >= 0, "error allocating samples: " << ret);
			}
				
			double ts = timeFromPts(frame->pkt_dts != AV_NOPTS_VALUE ? frame->pkt_pts : frame->pkt_dts, stream->time_base);

			int dstSampleCount = av_rescale_rnd(frame->nb_samples, freq, aCodecCtx->sample_rate, AV_ROUND_UP);

			int ret = swr_convert(swr, dstBuf, dstSampleCount, (const uint8_t**)frame->data, frame->nb_samples);

			if(ret >= 0 && addToQueue){
				device->Lock(true);
				for(int i = 0; i < dstSampleCount; i++){
					Sample smp;
					smp.chn[0] = ((int16_t*)dstBuf[0])[i * 2];
					smp.chn[1] = ((int16_t*)dstBuf[0])[i * 2 + 1];
					smp.ts = ts;
					smp.frameIndex = frameIndex;
					queue.push(smp);
				}
				device->Lock(false);
			}
			
			frameIndex++;
		}

		av_frame_free(&frame);

		return bytesDecoded;
	}
	
	void onSeek()
	{
		skip = true;
	}

	int getSampleRate(){
		if(aCodecCtx){
			return aCodecCtx->sample_rate;
		}
		return 0;
	}

	int getChannels(){
		if(aCodecCtx){
			return aCodecCtx->channels;
		}
		return 0;
	}

	int getBitRate(){
		if(aCodec){
			return aCodecCtx->bit_rate;
		}
		return 0;
	}

	const char* getCodec(){
		if(aCodec){
			return aCodec->name;
		}
		return "none";
	}
};

AudioHandlerPtr AudioHandler::Create(AVCodecContext* aCodecCtx, IAudioDevicePtr device, TimeHandlerPtr timeHandler)
{
	return AudioHandlerPtr(new CAudioHandler(aCodecCtx, device, timeHandler));
}