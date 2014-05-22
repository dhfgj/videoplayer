/*
 * NetClean VideoPlayer
 *  Multi process video player for windows.
 *
 * Copyright (c) 2010-2013 NetClean Technologies AB
 * All Rights Reserved.
 *
 * This file is part of NetClean VideoPlayer.
 *
 * NetClean VideoPlayer is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * NetClean VideoPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with NetClean VideoPlayer.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdlib>

#include "Player.h"
#include "Video.h"
#include "Common.h"
#include "AudioHandler.h"
#include "Flog.h"
#include "Tools.h"

typedef std::pair<std::string, std::string> Message;

void Player::AudioCallback(void *vMe, Uint8 *stream, int len)
{
	Player* me = (Player*)vMe;
	SampleQueue* samples = &me->samples;

	bool paused = me->video != 0 ? me->video->getPaused() : true;

	double videoTime = me->video != 0 ? me->video->getTime() : 0;
	double extraTime = 0;
	
	for(int i = 0; i < len; i += 4){
		extraTime += 1.0 / (double)me->freq;

		if(!samples->empty() && !paused){
			Sample s = samples->front();
			samples->pop();

			double adjust = s.ts - (videoTime + extraTime);
			if(adjust > 0)
				extraTime += adjust;
		
			for(int j = 0; j < 2; j++){
				s.chn[j] = (me->mute || me->quickViewPlayer) ? 0 : (int16_t)((float)s.chn[j] * me->volume);
			}

			stream[i+0] = s.chn[0] & 0xff;
			stream[i+1] = (s.chn[0] >> 8) & 0xff; 
			
			stream[i+2] = s.chn[1] & 0xff;
			stream[i+3] = (s.chn[1] >> 8) & 0xff;
		}else{
			stream[i] = stream[i+1] = stream[i+2] = stream[i+3] = 0;
		}

		if(extraTime >= 1.0 / 120.0 && me->video != 0){
			me->video->addTime(extraTime);
			extraTime = 0;
			videoTime = me->video->getTime();
		}
	}

	if(me->video != 0)
		me->video->addTime(extraTime);
}

Uint32 AudioFallbackTimer(Uint32 interval, void* data)
{
	Uint8 buffer[48 * 15 * 2 * 2]; // every 15 ms, so 48 KHz 16 bit stereo
	Player::AudioCallback(data, buffer, sizeof(buffer));
	return interval;
}

void Player::PauseAudio(bool val)
{
	if(!audioOutputEnabled)
		return;

	if(!val) InitAudio();
	else CloseAudio();
}

void Player::CloseAudio()
{
	if(!initialized) return;
	FlogD("closing audio");

	if(!audioOutputEnabled)
		SDL_RemoveTimer(noAudioTimer);

	SDL_CloseAudio();
	initialized = false;
}

void Player::InitAudio()
{
	if(initialized) return;
	FlogD("inititalizing audio");

	SDL_AudioSpec fmt, out;

	memset(&fmt, 0, sizeof(SDL_AudioSpec)); 
	memset(&out, 0, sizeof(SDL_AudioSpec)); 

	fmt.freq = 48000;
	fmt.format = AUDIO_S16LSB;
	fmt.channels = 2;
	fmt.samples = 1024;
	fmt.callback = AudioCallback;
	fmt.userdata = this;

	if(SDL_OpenAudio(&fmt, &out) != 0){
		FlogI("Could not open audio. Using fallback.");
		audioOutputEnabled = false;

		noAudioTimer = SDL_AddTimer(15, AudioFallbackTimer, this);
		FlogAssert(noAudioTimer != 0, "could not initialize audio timer");
		
		//SDL_CreateThread(AudioFallbackThread, this);

		initialized = true;
		return;
	}

	audioOutputEnabled = true;

	if(out.format != fmt.format){
		FlogW("audio format mismatch");
	}else{
		FlogI("audio format accepted");
	}

	FlogExpD(out.freq);
	FlogExpD(out.samples);
	
	initialized = true;

	freq = out.freq;

	SDL_PauseAudio(false);
}

void Player::Run(IpcMessageQueuePtr ipc, intptr_t handle)
{
	SDL_putenv("SDL_AUDIODRIVER=dsound");
	SDL_putenv(Str("SDL_WINDOWID=" << handle).c_str());

	//SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO);
	SDL_Init(SDL_INIT_EVERYTHING);

	screen = SDL_SetVideoMode(720, 480, 0, SDL_RESIZABLE);
	FlogAssert(screen, "could not create screen");

	initialized = false;
	std::queue<Message> sendQueue;

	quickViewPlayer = mute = false;
	volume = 1;

	bool running = true;
	bool redraw = true;
	bool active = false;
	bool clearScreen = false;

	InitAudio();	
	PauseAudio(true);

	x = y = 0;
	winw = w = screen->w;
	winh = h = screen->h;
	freq = 48000;

	SDL_Overlay* overlay = 0;

	while(running){
		//int t = SDL_GetTicks();

		SDL_Event event;
		while(SDL_PollEvent(&event)){
			if(event.type == SDL_QUIT)
				running = false;
		}

		for(unsigned int i = 0; i < sendQueue.size(); i++){
			// TODO background thread?
			//ipc->WritePacket(sendQueue.front().first, sendQueue.front().second /*, 1*/)){
			//	sendQueue.pop();
			
			ipc->WritePacket(sendQueue.front().first, sendQueue.front().second);
		}

		std::string type, message;

		while(ipc->ReadPacket(type, message)){
			FlogExpD(type);
			if(type == "load"){
				SDL_LockAudio();

				samples.clear();

				video = Video::Create(message, 
					// error handler
					[&](Video::Error error, const std::string& msg){
						if(error < Video::EEof)
							ipc->WritePacket("error", msg);
						else
							ipc->WritePacket(error == Video::EEof ? "eof" : "unloaded", msg);
					},
					// audio handler
					[&](const Sample* buffer, int size){
						SDL_LockAudio();
						for(int i = 0; i < size; i++)
							samples.push(buffer[i]);
						SDL_UnlockAudio();
					}, 48000, 2, quickViewPlayer ? 5 : 60);

				SDL_UnlockAudio();

				if(video){
					FlogD("loaded");
					ipc->WritePacket("loaded", "");
				}
			}
			
			else if(type == "setquickviewplayer"){
				quickViewPlayer = (message == "true");
				FlogExpD(quickViewPlayer);
			}

			else if(type == "quit"){
				running = false;
			}

			else if(type == "setactive"){
				active = (message == "true");
				FlogExpD(active);
			}

			if(video){
				std::stringstream s(message);
				
				if(type == "mute"){
					FlogD(type);
					FlogExpD(message == "true");
					mute = (message == "true");
				}

				if(type == "setplaybackspeed"){
					float speed = 1;
					s >> speed;
					FlogExpD(speed);
					video->setPlaybackSpeed(speed);
				}

				else if(type == "volume"){
					FlogD(type);
					float vol;
					s >> vol;
					FlogExpD(vol);
					vol = std::max(std::min(1.0f, vol), 0.0f);
					volume = log10(vol * 9 + 1);
					FlogExpD(volume);
				}

				else if(type == "unload")
				{
					FlogD(type);
					SDL_LockAudio();
					video = 0;
					SDL_UnlockAudio();
				}

				else if(type == "setkeyframes"){
					video->setIFrames(message);
				}

				else if(type == "getkeyframes"){
					ipc->WritePacket("keyframes", video->getIFrames());
				}

				else if(type == "play") {
					PauseAudio(false);
					video->play(); 
					FlogD("play");
				}

				else if(type == "pause") {
					video->pause(); 
					PauseAudio(true);
					FlogD("pause");	
				}

				else if(type == "getinfo") {
					ipc->WritePacket("dimensions", Str(video->getWidth() << " " << Str(video->getHeight())));
					ipc->WritePacket("videocodec", video->getVideoCodecName());
					ipc->WritePacket("format", video->getFormat());
					ipc->WritePacket("duration_in_frames", Str(video->getDurationInFrames()));
					ipc->WritePacket("reported_duration_in_frames", Str(video->getReportedDurationInFrames()));
					ipc->WritePacket("framerate", Str(video->getFrameRate()));
					ipc->WritePacket("reported_framerate", Str(video->getReportedFrameRate()));					
					ipc->WritePacket("aspectratio", Str(video->getAspect()));
					ipc->WritePacket("reported_length_in_secs", Str(video->getReportedDurationInSecs()));
					ipc->WritePacket("length_in_secs", Str(video->getDurationInSecs()));
					ipc->WritePacket("sample_rate", Str(video->getSampleRate()));
					ipc->WritePacket("audio_num_channels", Str(video->getNumChannels()));
				}

				else if(type == "step"){
					FlogD("step " << message);
					if(message == "back") video->stepBack();
					else video->step();
				}
				
				else if(type == "seek") {
					float t;
					s >> t;
					FlogD("seek " << t);
					samples.clear();
					video->seek(t * (float)video->getDurationInFrames(), true);
				}

				else if(type == "setdims"){
					s >> winw;
					s >> winh;
					s >> x;
					s >> y;
					s >> w;
					s >> h;

					FlogExpD(winw);
					FlogExpD(winh);
					FlogExpD(x);
					FlogExpD(y);
					FlogExpD(w);
					FlogExpD(h);

					clearScreen = true;
				}

				else if(type == "snapshot"){
					FlogD("snapshot");
					Frame currentFrame = video->getCurrentFrame();
					
					uint16_t w = video->getWidth(), h = video->getHeight();
					
					size_t bufferSize = avpicture_get_size(PIX_FMT_BGR24, w, h) + 4;
					char* buffer = (char*)av_malloc(bufferSize);

					*((uint16_t*)buffer) = w;
					*(((uint16_t*)buffer) + 1) = h;

					if(currentFrame.avFrame){
						FlogD("snapshot w: " << w << " h: " << h);

						uint8_t* buffers[] = {(uint8_t*)buffer + 4};
						video->frameToOverlay(currentFrame, Video::FRGB24, buffers, w, h);
						ipc->WritePacket("snapshot", std::string(buffer, bufferSize));

						FlogD("sent snapshot data");
					} else {
						FlogW("no frame to send");
					}

					av_free(buffer);
				}
			}

			// !video
			else {
				if(type == "getkeyframes"){
					ipc->WritePacket("keyframes", "error");
				}
			}
		}

		//FlogExpD(SDL_GetTicks() - t);

		if(active){
			if(video){
				video->tick();
				video->adjustTime();

				Frame frame = video->fetchFrame();

				if(frame.avFrame){
					int vw = video->getWidth();
					int vh = video->getHeight();

					// if there's an overlay that doesn't match the video dimensions, delete it
					if(overlay != 0 && (overlay->w != vw || vh != video->getHeight())){
						SDL_FreeYUVOverlay(overlay);
						overlay = 0;
					}

					// if there's no overlay, create one that matches the video dimensions
					if(overlay == 0)
						overlay = SDL_CreateYUVOverlay(vw, vh, SDL_YV12_OVERLAY, screen);

					// write framedata to overlay
					SDL_LockYUVOverlay(overlay);
					video->frameToOverlay(frame, Video::FYUV420P, overlay->pixels, vw, vh);
					SDL_UnlockYUVOverlay(overlay);

					ipc->WritePacket("position", Str((float)video->getPosition() / (float)video->getDurationInFrames()));
				}
			}
			
			// if the screen surface is smaller than the requested output size, resize the screen surface
			if(screen->w < winw || screen->h < winh){
				//SDL_FreeSurface(screen);
				FlogD("resizing screen surface");
				screen = SDL_SetVideoMode(winw + 100, winh + 100, 0, SDL_RESIZABLE);
				FlogAssert(screen, "could not create screen");
			}
				
			if(overlay != 0){
				int vw = overlay->w;
				int vh = overlay->h;

				int nw = std::max(std::min(w, 1920), 64);
				int nh = std::max(std::min(h, 1080), 64);

				// Keep aspect ratio
				float zoom = std::min((float)nw / (float)vw, (float)nh / (float)vh);

				nw = (float)vw * zoom;
				nh = (float)vh * zoom;

				SDL_Rect r = {(int16_t)(x - (nw - w) / 2), (int16_t)(y - (nh - h) / 2), (uint16_t)nw, (uint16_t)nh};
				SDL_DisplayYUVOverlay(overlay, &r);
			}

			if(redraw || clearScreen){
				if(clearScreen){
					SDL_FillRect(screen, 0, 0);
					clearScreen = false;
				}

				SDL_Flip(screen);
				redraw = false;
			}
		}
		
		SDL_Delay(video && !video->getPaused() ? 8 : 100);
	}

	CloseAudio();
	FlogD("exiting");
}