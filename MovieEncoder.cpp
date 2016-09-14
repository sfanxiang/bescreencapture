#include <Bitmap.h>
#include <Debug.h>
#include <List.h>
#include <Screen.h>
#include <String.h>
#include <TranslationUtils.h>
#include <View.h>

#include <cstring>

#include "BitmapMovie.h"
#include "Executor.h"
#include "FileList.h"
#include "messages.h"
#include "MovieEncoder.h"
#include "Utils.h"


MovieEncoder::MovieEncoder()
	:
	fEncoderThread(-1),
	fKillThread(false),
	fFileList(NULL),
	fCursorQueue(NULL),
	fColorSpace(B_NO_COLOR_SPACE)
{
}


MovieEncoder::~MovieEncoder()
{
	DisposeData();
}


void
MovieEncoder::DisposeData()
{
	fFileList = NULL;
}


void
MovieEncoder::Cancel()
{
	if (fEncoderThread > 0) {
		fKillThread = true;
		status_t dummy;
		wait_for_thread(fEncoderThread, &dummy);
	}
}


status_t
MovieEncoder::SetSource(const FileList *fileList)
{
	fFileList = fileList;
	return B_OK;
}


status_t
MovieEncoder::SetOutputFile(const char *fileName)
{
	fOutputFile.SetTo(fileName);
	return B_OK;
}


status_t
MovieEncoder::SetDestFrame(const BRect &rect)
{
	fDestFrame = rect;
	fDestFrame.OffsetTo(B_ORIGIN);
	
	return B_OK;
}


void
MovieEncoder::SetColorSpace(const color_space &space)
{
	fColorSpace = space;
}


status_t
MovieEncoder::SetQuality(const float &quality)
{
	return B_OK;
}


status_t
MovieEncoder::SetCursorQueue(std::queue<BPoint> *queue)
{
	fCursorQueue = queue;
	return B_OK;
}


status_t
MovieEncoder::SetMessenger(const BMessenger &messenger)
{
	if (!messenger.IsValid())
		return B_BAD_VALUE;
	
	fMessenger = messenger;
	return B_OK;
}


// When this is running, no member variable should be accessed
// from other threads
status_t
MovieEncoder::_EncoderThread()
{	
	int32 framesLeft = fFileList->CountItems();
	int32 framesWritten = 0;
	
	if (framesLeft <= 0) {
		DisposeData();
		BMessage message(kEncodingFinished);
		message.AddInt32("status", (int32)B_ERROR);
		fMessenger.SendMessage(&message);
		return B_ERROR;
	}
	
	// Create movie
	entry_ref movieRef;
	get_ref_for_path(fOutputFile.Path(), &movieRef);
		
	BBitmap* bitmap = BTranslationUtils::GetBitmapFile(fFileList->ItemAt(0));
	BRect sourceFrame = bitmap->Bounds();
	delete bitmap;
		
	if (!fDestFrame.IsValid())
		fDestFrame = sourceFrame.OffsetToCopy(B_ORIGIN);
				
	BitmapMovie* movie = new BitmapMovie(fDestFrame.IntegerWidth() + 1,
					fDestFrame.IntegerHeight() + 1, fColorSpace);
				
	status_t status = movie->CreateFile(movieRef, fFileFormat, fFormat, fCodecInfo);
	if (status < B_OK) {
		delete movie;
		DisposeData();
		BMessage message(kEncodingFinished);
		message.AddInt32("status", (int32)status);
		fMessenger.SendMessage(&message);		
		return status;
	}
		
	// Bitmap and view used to convert the source bitmap
	// to the correct size and depth	
	BBitmap* destBitmap = new BBitmap(fDestFrame, fColorSpace, true);
	BView* destDrawer = new BView(fDestFrame, "drawing view", B_FOLLOW_NONE, 0);
	if (destBitmap->Lock()) {
		destBitmap->AddChild(destDrawer);
		destBitmap->Unlock();
	}
	
	const uint32 keyFrameFrequency = 10;
		// TODO: Make this tunable
	
	BMessage progressMessage(B_UPDATE_STATUS_BAR);
	progressMessage.AddFloat("delta", 1.0);
	
	status = B_OK;
	for (int32 i = 0; i < fFileList->CountItems(); i++) {
		if (fKillThread)
			break;
			
		bool keyFrame = (framesWritten % keyFrameFrequency == 0);
		BString fileName = fFileList->ItemAt(i);
		BBitmap* frame = BTranslationUtils::GetBitmapFile(fileName);
		if (frame == NULL) {
			// TODO: What to do here ? Exit with an error ?
			continue;
		}
						
		// Draw scaled
		if (status == B_OK) {
			destBitmap->Lock();
			destDrawer->DrawBitmap(frame, frame->Bounds(), destDrawer->Bounds());
			destDrawer->Sync();
			destBitmap->Unlock();
		}
		
		delete frame;
			
		if (status == B_OK)
			status = movie->WriteFrame(destBitmap, keyFrame);
		
		if (status != B_OK)
			break;

		framesWritten++;

		if (fMessenger.IsValid())
			fMessenger.SendMessage(new BMessage(progressMessage));
		else {
			// BMessenger is no longer valid. This means that the application
			// has been closed or it has crashed.
			break;
		}
	}

	delete movie;
	delete destBitmap;
	
	DisposeData();
	
	if (fMessenger.IsValid()) {
		BMessage message(kEncodingFinished);
		message.AddInt32("status", (int32)status);
		message.AddInt32("frames", (int32)framesWritten);
		fMessenger.SendMessage(&message);
	}
		
	return status;
}


thread_id
MovieEncoder::EncodeThreaded()
{
	fKillThread = false;
	
	fEncoderThread = spawn_thread((thread_entry)EncodeStarter,
		"Encoder Thread", B_DISPLAY_PRIORITY, this);
					
	if (fEncoderThread < 0)
		return fEncoderThread;
	
	status_t status = resume_thread(fEncoderThread);
	if (status < B_OK) {
		kill_thread(fEncoderThread);
		return status;
	}
	
	return fEncoderThread;
}



void
MovieEncoder::ResetConfiguration()
{
	fColorSpace = B_NO_COLOR_SPACE;
}


media_format_family
MovieEncoder::MediaFormatFamily() const
{
	return fFamily;
}


void
MovieEncoder::SetMediaFormatFamily(const media_format_family& family)
{
	fFamily = family;
}


media_file_format
MovieEncoder::MediaFileFormat() const
{
	return fFileFormat;
}


void
MovieEncoder::SetMediaFileFormat(const media_file_format& fileFormat)
{
	fFileFormat = fileFormat;
}


media_format
MovieEncoder::MediaFormat() const
{
	return fFormat;
}


void
MovieEncoder::SetMediaFormat(const media_format& format)
{
	fFormat = format;
}


void
MovieEncoder::SetMediaCodecInfo(const media_codec_info& info)
{
	fCodecInfo = info;
}


media_codec_info
MovieEncoder::MediaCodecInfo() const
{
	return fCodecInfo;
}


// private methods
BBitmap*
MovieEncoder::GetCursorBitmap(const uint8* cursor)
{
	uint8 size = *cursor;
	
	BBitmap* cursorBitmap = new BBitmap(BRect(0, 0, size - 1, size - 1), B_RGBA32);
	
	uint32 black = 0xFF000000;
	uint32 white = 0xFFFFFFFF;
	
	uint8* castCursor = const_cast<uint8*>(cursor);
	uint16* cursorPtr = reinterpret_cast<uint16*>(castCursor + 4);
	uint16* maskPtr = reinterpret_cast<uint16*>(castCursor + 36);
	uint8* buffer = static_cast<uint8*>(cursorBitmap->Bits());
	uint16 cursorFlip, maskFlip;
	uint16 cursorVal, maskVal;
	for (uint8 row = 0; row < size; row++) {
		uint32* bits = (uint32*)(buffer + (row * cursorBitmap->BytesPerRow()));
		cursorFlip = (cursorPtr[row] & 0xFF) << 8;
		cursorFlip |= (cursorPtr[row] & 0xFF00) >> 8;
		
		maskFlip = (maskPtr[row] & 0xFF) << 8;
		maskFlip |= (maskPtr[row] & 0xFF00) >> 8;
		
		for (uint8 column = 0; column < size; column++) {
			uint16 posVal = 1 << (15 - column);
			cursorVal = cursorFlip & posVal;
			maskVal = maskFlip & posVal;
			bits[column] = (cursorVal != 0 ? black : white) &
							(maskVal > 0 ? white : 0x00FFFFFF);
							
		}
	}
		
	return cursorBitmap;
}


status_t
MovieEncoder::PopCursorPosition(BPoint& point)
{
	ASSERT(fCursorQueue != NULL);
	//point = fCursorQueue->front();
	//fCursorQueue->pop();
	
	return B_OK;
}


int32
MovieEncoder::EncodeStarter(void* arg)
{
	return static_cast<MovieEncoder*>(arg)->_EncoderThread();
}

