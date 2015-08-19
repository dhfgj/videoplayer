#ifndef COMMAND_QUEUE_H
#define COMMAND_QUEUE_H

#include <memory>
#include <string>
#include <vector>

#include "Pipe.h"

#define MAGIC 0xaabbaacc

typedef std::shared_ptr<class CommandQueue> CommandQueuePtr;

class CommandQueueException : public std::runtime_error {
	public:
	CommandQueueException(std::string str) : std::runtime_error(str) {}
};

enum CommandType
{
	CTQuit =             0,
	CTPlay =             1,
	CTPause =            2, 
	CTStop =             3,
	CTSeek =             4,
	CTLoad =             5,
	CTUnload =           6,
	CTLfsConnect =       7,
	CTLfsDisconnect =    8,
	CTUpdateOutputSize = 9,
	CTPositionUpdate =  10,
	CTDuration       =  11,

	CTCmdCount
};

enum LoadType
{
	LTFile,
	LTLfs
};

enum ArgumentType
{
	ATStr, ATInt32, ATFloat, ATDouble
};

// argument type specification for the different commands
const std::vector<std::vector<ArgumentType>> CommandArgs  = {{}, {}, {}, {}, {ATFloat}, {ATInt32, ATStr}, {}, {ATStr}, {}, {ATInt32, ATInt32}, {ATFloat}, {ATFloat}};

struct Argument
{
	ArgumentType type;

	int32_t i;
	std::wstring str;
	float f;
	double d;
};

struct Command
{
	CommandType type;
	std::vector<Argument> args;
};

class CommandQueue
{
	public:
	virtual void Start(PipePtr pipe) = 0;
	virtual bool Dequeue(Command& cmd) = 0;
	virtual void WaitForConnection(int msTimeout) = 0;

	static CommandQueuePtr Create();
};

#endif