#include "dbgexts.h"
extern IDebugClient* pDebugClient;
extern IDebugControl* pDebugControl;

#include "windbgshark.h"

#include "pcap.h"

#include "utils.h"

void printLastError();

IDebugBreakpoint *bpIn, *bpOut;

HANDLE hPcapWatchdog = INVALID_HANDLE_VALUE;
HANDLE hWatchdogTerminateEvent = INVALID_HANDLE_VALUE;

HRESULT setBreakpoints(PDEBUG_CONTROL Control);
HRESULT removeBreakpoints(PDEBUG_CONTROL Control);

BOOL modeStepTrace = FALSE;
BOOL Debug = FALSE;

#undef _CTYPE_DISABLE_MACROS

void windbgsharkInit()
{
	myDprintf("windbgsharkInit...\n");

	myDprintf("setBreakpoints...\n");
	setBreakpoints(pDebugControl);

	myDprintf("openPcap...\n");
	openPcap();

	myDprintf("starting wireshark...\n");
	startWireshark();

	myDprintf("loading symbols for the driver\n");
	pDebugControl->Execute(
			DEBUG_OUTCTL_IGNORE | DEBUG_OUTCTL_NOT_LOGGED,
			".reload windbgshark_drv.sys",
			DEBUG_EXECUTE_NOT_LOGGED);
	

	myDprintf("Creating sync objects...\n");
	hWatchdogTerminateEvent = CreateEvent(NULL, FALSE, FALSE, NULL); 

	if(hWatchdogTerminateEvent == NULL)
	{
		// unload?
	}
}

void windbgsharkUninitialize()
{
	myDprintf("windbgsharkUninitialize: calling removeBreakpoints...\n");
	removeBreakpoints(pDebugControl);

	myDprintf("windbgsharkUninitialize: calling hPcapWatchdog...\n");
	terminateWatchdog();

	myDprintf("windbgsharkUninitialize: releasing the objects...\n");
	if(pDebugClient) pDebugClient->Release();
    if(pDebugControl) pDebugControl->Release();

	stopWireshark();

	closePcap();
}


HRESULT CALLBACK
help(PDEBUG_CLIENT4 Client, PCSTR args)
{
    dprintf("Heya, I'm Windbgshark.\n\n");
	dprintf("You may user the following commands:\n");

	dprintf("!strace \t\t\t show help\n");
	dprintf("!strace \t\t\t show the current mode (step-trace or pass-through)\n");
	dprintf("!strace {on|off} \t\t turn on/off the step-trace mode \n");
	dprintf("!packet \t\t\t show the current packet in hex dump \n");
	dprintf("!packet <size> \t\t set the current packet size (either truncates or enlarges the packet)\n");
	dprintf("!packet <offset> <string> \t replace the contents at <offset> by <string>\n");
	dprintf("!packet <offset> +<string> \t insert the <string> at <offset> (enlarges the packet)\n");
	dprintf("!packet <offset> -<size> \t remove <size> characters at <offset> (reduces packet size)\n");
	
	dprintf("\nHints:\n");
	dprintf("- all input numbers are treated as hex\n");
	dprintf("- all standard escape sequences (\\n, \\x41 and so on) in input strings are converterted in to the corresponding characters\n");

	return S_OK;
}

void printIncorrectArgs(PCSTR args)
{
	dprintf("Sorry, cannot parse arguments: %s", args);
}

HRESULT CALLBACK
packet(PDEBUG_CLIENT4 Client, PCSTR args)
{
	UINT32 argsLen = strlen(args);

	// ex: !packet
	if(args == NULL || argsLen == 0)
	{
		showPacket();
		return S_OK;
	}

	// ex: !packet abc
	if(!isxdigit(args[0]))
	{
		printIncorrectArgs(args);
		return S_OK;
	}

	char *endSizePtr = NULL;
	UINT32 offset = strtol(args, &endSizePtr, 16);

	// ex: !packet 20
	if(endSizePtr == NULL || *endSizePtr == '\0')
	{
		setPacketSize(offset);
	}
	else if(*endSizePtr == ' ')
	{
		if(endSizePtr + 1 < args + argsLen && *(endSizePtr + 1) == '+')
		{
			// ex: !packet 20 +quick brown fox\x41\x41\x41
			if(endSizePtr + 2 < args + argsLen)
			{
				UINT32 unescapeDataLength = argsLen - (endSizePtr - args) - 2;
				char *unescapeData = new char[unescapeDataLength + 1];
				unescape(unescapeData, endSizePtr + 2);
				unescapeDataLength = strlen(unescapeData);
				insertDataAtPacketOffset(offset, unescapeData, unescapeDataLength);

				if(unescapeData != NULL)
				{
					delete [] unescapeData;
				}
			}
		}
		else if(endSizePtr + 1 < args + argsLen && *(endSizePtr + 1) == '-')
		{
			// ex: !packet 20 -10
			if(endSizePtr + 2 < args + argsLen && isxdigit(*(endSizePtr + 2)))
			{
				char *endCutSizePtr = NULL;
				UINT32 cutSize = strtol(endSizePtr + 2, &endCutSizePtr, 16);
				cutDataAtPacketOffset(offset, cutSize);
			}
			else
			{
				printIncorrectArgs(args);
				return S_OK;
			}
		}
		else if(endSizePtr + 1 < args + argsLen)
		{
			// ex: !packet 20 quickbrown fox \n\n\x41
			UINT32 unescapeDataLength = argsLen - (endSizePtr - args) - 1;
			char *unescapeData = new char[unescapeDataLength + 1];
			unescape(unescapeData, endSizePtr + 1);
			unescapeDataLength = strlen(unescapeData);
			cutDataAtPacketOffset(offset, unescapeDataLength);
			insertDataAtPacketOffset(offset, unescapeData, unescapeDataLength);
		}
		else
		{
			printIncorrectArgs(args);
			return S_OK;
		}
	}
	else
	{
		printIncorrectArgs(args);
		return S_OK;
	}
	
	

	showPacket();


	return S_OK;
}

HRESULT CALLBACK
strace(PDEBUG_CLIENT4 Client, PCSTR args)
{
	INIT_API();
	
	// myDprintf("strace: args = %s (%p), strlen(args) = %d, strcmp(args, \"on\") = %d\n", args, args, strlen(args), strcmp(args, "on"));

	if(args != NULL && strlen(args) > 0)
	{
		if(strlen(args) == 2 && strcmp(args, "on") == 0)
		{
			dprintf("enabled packet step tracing (break)\n");
			modeStepTrace = TRUE;
			bpIn->SetCommand("!onpacketinspect; .printf \"!packet\\n\"; !packet");
		}
		else if(strlen(args) == 3 && strcmp(args, "off") == 0)
		{
			dprintf("disabled packet step tracing (pass-through)\n");
			modeStepTrace = FALSE;
			bpIn->SetCommand("!onpacketinspect; g");
		}
	}
	else
	{
		dprintf("packet step tracing - ");

		if(modeStepTrace)
		{
			dprintf("enabled (break)\n");
		}
		else
		{
			dprintf("disabled (pass-through)\n");
		}
	}

	EXIT_API();

	return S_OK;
}

HRESULT CALLBACK
onpacketinspect(PDEBUG_CLIENT4 Client, PCSTR args)
{
	INIT_API();

	myDprintf("onpacketinspect: Enter----------------------------------------\n");

	fixCurrentPcapSize();

	composePcapRecord();

	if(modeStepTrace)
	{
		if(hPcapWatchdog != INVALID_HANDLE_VALUE)
		{
			terminateWatchdog();
		}

		ResetEvent(hWatchdogTerminateEvent);

		hPcapWatchdog = CreateThread(
			NULL,
			0,
			(LPTHREAD_START_ROUTINE ) &feedPcapWatchdog,
			NULL,
			0,
			0);	
	}

	myDprintf("onpacketinspect: Cleanup--------------------------------------\n");
	EXIT_API();

	return S_OK;
}

HRESULT CALLBACK
onpacketinject(PDEBUG_CLIENT4 Client, PCSTR args)
{
	INIT_API();

	myDprintf("onpacketinject: Enter---------------------------------------\n");

	terminateWatchdog();

	myDprintf("onpacketinject: Cleanup-------------------------------------\n");

	EXIT_API();

	return S_OK;
}

HRESULT
setBreakpoints(PDEBUG_CONTROL Control)
{
	HRESULT result = Control->AddBreakpoint(
		DEBUG_BREAKPOINT_CODE,
		DEBUG_ANY_ID,
		&bpIn);

	result = bpIn->SetOffsetExpression("windbgshark_drv!inspectPacket+0xd4");

	if(modeStepTrace)
	{
		result = bpIn->SetCommand("!onpacketinspect; .printf \"!packet\\n\"; !packet");
	}
	else
	{
		result = bpIn->SetCommand("!onpacketinspect; g");
	}

	result = bpIn->SetFlags(DEBUG_BREAKPOINT_ENABLED);

	result = Control->AddBreakpoint(
		DEBUG_BREAKPOINT_CODE,
		DEBUG_ANY_ID,
		&bpOut);

	result = bpOut->SetOffsetExpression("windbgshark_drv!inspectPacket+0xcf");
	result = bpOut->SetCommand("!onpacketinject; g");
	result = bpOut->SetFlags(DEBUG_BREAKPOINT_ENABLED);

	return S_OK;
}

HRESULT
removeBreakpoints(PDEBUG_CONTROL Control)
{
	HRESULT result;

	result = Control->RemoveBreakpoint(bpIn);
	result = Control->RemoveBreakpoint(bpOut);

	return S_OK;
}

void printLastError()
{
    LPCTSTR lpMsgBuf;

    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR) & lpMsgBuf, 0, NULL);

	myDprintf(lpMsgBuf);

    LocalFree((HLOCAL) lpMsgBuf);
}


