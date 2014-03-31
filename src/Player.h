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

#ifndef PLAYER_H
#define PLAYER_H

#include <cstdint>
#include <SDL.h>

#include "IpcMessageQueue.h"
#include "SampleQueue.h"
#include "Video.h"

class Player
{
	SampleQueue samples;

	void InitAudio();
	void CloseAudio();

	bool initialized;
	int x, y, w, h;
	int winw, winh;
		
	float volume;
	bool mute, quickViewPlayer, audioOutputEnabled;
	SDL_TimerID noAudioTimer;
	SDL_Surface* screen;

	public:
	VideoPtr video;
	int freq;

	void AddVideoTime(double t);
	void PauseAudio(bool val);
	void Run(IpcMessageQueuePtr ipc, intptr_t handle);
	static void AudioCallback(void *me, Uint8 *stream, int len);
};

#endif
