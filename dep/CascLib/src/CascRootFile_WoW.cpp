/*****************************************************************************/
/* CascRootFile_WoW.cpp                   Copyright (c) Ladislav Zezula 2014 */
/*---------------------------------------------------------------------------*/
/* Storage functions for CASC                                                */
/* Note: WoW offsets refer to WoW.exe 6.0.3.19116 (32-bit)                   */
/* SHA1: c10e9ffb7d040a37a356b96042657e1a0c95c0dd                            */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 29.04.14  1.00  Lad  The first version of CascRootFile_WoW.cpp            */
/*****************************************************************************/

#define __CASCLIB_SELF__
#include "CascLib.h"
#include "CascCommon.h"

//-----------------------------------------------------------------------------
// Local structures

#define ROOT_SEARCH_PHASE_INITIALIZING  0
#define ROOT_SEARCH_PHASE_LISTFILE      1
#define ROOT_SEARCH_PHASE_NAMELESS      2
#define ROOT_SEARCH_PHASE_FINISHED      3

// Known dwRegion values returned from sub_661316 (7.0.3.22210 x86 win), also referred by lua GetCurrentRegion
#define WOW_REGION_US              0x01
#define WOW_REGION_KR              0x02
#define WOW_REGION_EU              0x03
#define WOW_REGION_TW              0x04
#define WOW_REGION_CN              0x05

typedef enum _ROOT_FORMAT
{
    RootFormatWoW_v1,                           // Since build 18125 (WoW 6.0.1)
    RootFormatWoW_v2,                           // Since build 30080 (WoW 8.2.0)
} ROOT_FORMAT, *PROOT_FORMAT;

// The last byte of the structure causes wrong alignment with default compiler options
#pragma pack(push, 1)
typedef struct _FILE_ROOT_GROUP_HEADER_58221    // Since build 58221 (11.1.0.58221)
{
    DWORD NumberOfFiles;                        // Number of entries
    DWORD LocaleFlags;                          // File locale mask (CASC_LOCALE_XXX)
    DWORD ContentFlags1;
    DWORD ContentFlags2;
    BYTE ContentFlags3;

} FILE_ROOT_GROUPHEADER_58221, *PFILE_ROOT_GROUPHEADER_58221;
#pragma pack(pop)

// ROOT file header since build 50893 (10.1.7)
typedef struct _FILE_ROOT_HEADER_50893
{
    DWORD Signature;                            // Must be CASC_WOW_ROOT_SIGNATURE
    DWORD SizeOfHeader;
    DWORD Version;                              // Must be 1
    DWORD TotalFiles;
    DWORD FilesWithNameHash;
} FILE_ROOT_HEADER_50893, *PFILE_ROOT_HEADER_50893;

// ROOT file header since build 30080 (8.2.0)
typedef struct _FILE_ROOT_HEADER_30080
{
    DWORD Signature;                            // Must be CASC_WOW_ROOT_SIGNATURE
    DWORD TotalFiles;
    DWORD FilesWithNameHash;
} FILE_ROOT_HEADER_30080, *PFILE_ROOT_HEADER_30080;

// On-disk version of root group. A root group contains a group of file
// with the same locale and file flags
typedef struct _FILE_ROOT_GROUP_HEADER
{
    DWORD NumberOfFiles;                        // Number of entries
    DWORD ContentFlags;
    DWORD LocaleFlags;                          // File locale mask (CASC_LOCALE_XXX)

    // Followed by a block of file data IDs (count: NumberOfFiles)
    // Followed by the MD5 and file name hash (count: NumberOfFiles)

} FILE_ROOT_GROUP_HEADER, *PFILE_ROOT_GROUP_HEADER;

// On-disk version of root entry. Only present in versions 6.x - 8.1.xx
// Each root entry represents one file in the CASC storage
// In WoW build 30080 (8.2.0)+, CKey and FileNameHash are split into separate arrays
// and FileNameHash is optional
typedef struct _FILE_ROOT_ENTRY
{
    CONTENT_KEY CKey;                           // MD5 of the file
    ULONGLONG FileNameHash;                     // Jenkins hash of the file name

} FILE_ROOT_ENTRY, *PFILE_ROOT_ENTRY;

typedef struct _FILE_ROOT_GROUP
{
    FILE_ROOT_GROUP_HEADER Header;
    PDWORD FileDataIds;                         // Pointer to the array of File Data IDs

    PFILE_ROOT_ENTRY pRootEntries;              // Valid for WoW since 18125
    PCONTENT_KEY pCKeyEntries;                  // Valid for WoW since 30080
    PULONGLONG pHashes;                         // Valid for WoW since 30080 (optional)

} FILE_ROOT_GROUP, *PFILE_ROOT_GROUP;

//-----------------------------------------------------------------------------
// TRootHandler_WoW interface / implementation

#define FTREE_FLAGS_WOW (FTREE_FLAG_USE_DATA_ID | FTREE_FLAG_USE_LOCALE_FLAGS | FTREE_FLAG_USE_CONTENT_FLAGS)

struct TRootHandler_WoW : public TFileTreeRoot
{
    public:

    typedef LPBYTE (*CAPTURE_ROOT_HEADER)(LPBYTE pbRootPtr, LPBYTE pbRootEnd, PROOT_FORMAT RootFormat, PDWORD FileCounterHashless, PDWORD Version);

    TRootHandler_WoW(ROOT_FORMAT aRootFormat, DWORD aFileCounterHashless, LPCTSTR szDumpFile = NULL) : TFileTreeRoot(FTREE_FLAGS_WOW)
    {
        // Turn off the "we know file names" bit
        FileCounterHashless = aFileCounterHashless;
        FileCounter = 0;
        RootFormat = aRootFormat;
        fp = NULL;

        // Update the flags based on format
        switch(RootFormat)
        {
            case RootFormatWoW_v2:
                dwFeatures |= CASC_FEATURE_ROOT_CKEY | CASC_FEATURE_LOCALE_FLAGS | CASC_FEATURE_CONTENT_FLAGS | CASC_FEATURE_FILE_DATA_IDS | CASC_FEATURE_FNAME_HASHES_OPTIONAL;
                break;

            case RootFormatWoW_v1:
                dwFeatures |= CASC_FEATURE_ROOT_CKEY | CASC_FEATURE_LOCALE_FLAGS | CASC_FEATURE_CONTENT_FLAGS | CASC_FEATURE_FNAME_HASHES;
                break;
        }

        // Create the file for dumping listfile
        if(szDumpFile && szDumpFile[0])
        {
            fp = _tfopen(szDumpFile, _T("wt"));
        }
    }

    ~TRootHandler_WoW()
    {
        if(fp != NULL)
            fclose(fp);
        fp = NULL;
    }

#ifdef CASCLIB_WRITE_VERIFIED_FILENAMES
    void VerifyAndLogFileName(LPCSTR szFileName, ULONG FileDataId)
    {
        PCASC_FILE_NODE pFileNode;
        ULONGLONG FileNameHash = CalcFileNameHash(szFileName);
        
        if((pFileNode = FileTree.Find(FileNameHash)) != NULL)
        {
            if(pFileNode->FileNameHash == FileNameHash)
            {
                if(FileDataId != 0)
                    fprintf(fp, "%u;%s\n", FileDataId, szFileName);
                else
                    fprintf(fp, "%s\n", szFileName);
            }
        }
    }
#else
    #define VerifyAndLogFileName(szFileName, FileDataId)    /* */
#endif

    // Check for the new format (World of Warcraft 10.1.7, build 50893)
    static LPBYTE CaptureRootHeader_50893(LPBYTE pbRootPtr, LPBYTE pbRootEnd, PROOT_FORMAT RootFormat, PDWORD FileCounterHashless, PDWORD Version)
    {
        FILE_ROOT_HEADER_50893 RootHeader;

        // Validate the root file header
        if((pbRootPtr + sizeof(FILE_ROOT_HEADER_50893)) >= pbRootEnd)
            return NULL;
        memcpy(&RootHeader, pbRootPtr, sizeof(FILE_ROOT_HEADER_50893));

        // Verify the root file header
        if(RootHeader.Signature != CASC_WOW_ROOT_SIGNATURE)
            return NULL;
        if(RootHeader.Version != 1 && RootHeader.Version != 2)
            return NULL;
        if(RootHeader.FilesWithNameHash > RootHeader.TotalFiles)
            return NULL;
        // wow client doesn't seem to think this is a fatal error, we will do the same for now
        if(RootHeader.SizeOfHeader < 4)
            RootHeader.SizeOfHeader = 4;

        *RootFormat = RootFormatWoW_v2;
        *FileCounterHashless = RootHeader.TotalFiles - RootHeader.FilesWithNameHash;
        *Version = RootHeader.Version;
        return pbRootPtr + RootHeader.SizeOfHeader;
    }

    // Check for the root format for build 30080+ (WoW 8.2.0)
    static LPBYTE CaptureRootHeader_30080(LPBYTE pbRootPtr, LPBYTE pbRootEnd, PROOT_FORMAT RootFormat, PDWORD FileCounterHashless, PDWORD Version)
    {
        FILE_ROOT_HEADER_30080 RootHeader;

        // Validate the root file header
        if((pbRootPtr + sizeof(FILE_ROOT_HEADER_30080)) >= pbRootEnd)
            return NULL;
        memcpy(&RootHeader, pbRootPtr, sizeof(FILE_ROOT_HEADER_30080));

        // Verify the root file header
        if(RootHeader.Signature != CASC_WOW_ROOT_SIGNATURE)
            return NULL;
        if(RootHeader.FilesWithNameHash > RootHeader.TotalFiles)
            return NULL;

        *RootFormat = RootFormatWoW_v2;
        *FileCounterHashless = RootHeader.TotalFiles - RootHeader.FilesWithNameHash;
        *Version = 0;
        return pbRootPtr + sizeof(FILE_ROOT_HEADER_30080);
    }

    // Check for the root format for build 18125+ (WoW 6.0.1)
    static LPBYTE CaptureRootHeader_18125(LPBYTE pbRootPtr, LPBYTE pbRootEnd, PROOT_FORMAT RootFormat, PDWORD FileCounterHashless, PDWORD Version)
    {
        size_t DataLength;

        // There is no header. Right at the begin, there's FILE_ROOT_GROUP_HEADER structure,
        // followed by the array of DWORDs and FILE_ROOT_ENTRYs
        if((pbRootPtr + sizeof(FILE_ROOT_GROUP_HEADER)) >= pbRootEnd)
            return NULL;
        DataLength = ((PFILE_ROOT_GROUP_HEADER)(pbRootPtr))->NumberOfFiles * (sizeof(DWORD) + sizeof(FILE_ROOT_ENTRY));

        // Validate the array of data
        if((pbRootPtr + sizeof(FILE_ROOT_GROUP_HEADER) + DataLength) >= pbRootEnd)
            return NULL;

        *RootFormat = RootFormatWoW_v1;
        *FileCounterHashless = 0;
        *Version = 0;
        return pbRootPtr;
    }

    static LPBYTE CaptureRootHeader(LPBYTE pbRootPtr, LPBYTE pbRootEnd, PROOT_FORMAT RootFormat, PDWORD FileCounterHashless, PDWORD Version)
    {
        CAPTURE_ROOT_HEADER PfnCaptureRootHeader[] =
        {
            &CaptureRootHeader_50893,
            &CaptureRootHeader_30080,
            &CaptureRootHeader_18125,
        };

        for(size_t i = 0; i < _countof(PfnCaptureRootHeader); i++)
        {
            LPBYTE pbCapturedPtr;

            if((pbCapturedPtr = PfnCaptureRootHeader[i](pbRootPtr, pbRootEnd, RootFormat, FileCounterHashless, Version)) != NULL)
            {
                return pbCapturedPtr;
            }
        }
        return NULL;
    }

    LPBYTE CaptureRootGroup(FILE_ROOT_GROUP & RootGroup, LPBYTE pbRootPtr, LPBYTE pbRootEnd, DWORD dwRootVersion)
    {
        // Reset the entire root group structure
        memset(&RootGroup, 0, sizeof(FILE_ROOT_GROUP));

        if(dwRootVersion == 0 || dwRootVersion == 1)
        {
            // Validate the locale block header
            if((pbRootPtr + sizeof(FILE_ROOT_GROUP_HEADER)) >= pbRootEnd)
                return NULL;
            memcpy(&RootGroup.Header, pbRootPtr, sizeof(FILE_ROOT_GROUP_HEADER));
            pbRootPtr = pbRootPtr + sizeof(FILE_ROOT_GROUP_HEADER);
        }
        else if(dwRootVersion == 2)
        {
            PFILE_ROOT_GROUPHEADER_58221 pRootGroupHeader;
            
            // Get pointer to the root group header
            if((pbRootPtr + sizeof(FILE_ROOT_GROUPHEADER_58221)) >= pbRootEnd)
                return NULL;
            pRootGroupHeader = (PFILE_ROOT_GROUPHEADER_58221)pbRootPtr;
            pbRootPtr = pbRootPtr + sizeof(FILE_ROOT_GROUPHEADER_58221);

            // Convert to old ContentFlags for now...
            RootGroup.Header.NumberOfFiles = pRootGroupHeader->NumberOfFiles;
            RootGroup.Header.ContentFlags = pRootGroupHeader->ContentFlags1 | pRootGroupHeader->ContentFlags2 | (DWORD)(pRootGroupHeader->ContentFlags3 << 17);
            RootGroup.Header.LocaleFlags = pRootGroupHeader->LocaleFlags;
        }

        // Validate the array of file data IDs
        if((pbRootPtr + (sizeof(DWORD) * RootGroup.Header.NumberOfFiles)) >= pbRootEnd)
            return NULL;
        RootGroup.FileDataIds = (PDWORD)pbRootPtr;
        pbRootPtr = pbRootPtr + (sizeof(DWORD) * RootGroup.Header.NumberOfFiles);

        // Add the number of files in this block to the number of files loaded
        FileCounter += RootGroup.Header.NumberOfFiles;

        // Validate the array of root entries
        switch(RootFormat)
        {
            case RootFormatWoW_v2:

                // Verify the position of array of CONTENT_KEY
                if((pbRootPtr + (sizeof(CONTENT_KEY) * RootGroup.Header.NumberOfFiles)) > pbRootEnd)
                    return NULL;
                RootGroup.pCKeyEntries = (PCONTENT_KEY)pbRootPtr;
                pbRootPtr = pbRootPtr + (sizeof(CONTENT_KEY) * RootGroup.Header.NumberOfFiles);

                // Also include array of file hashes
                if(!(RootGroup.Header.ContentFlags & CASC_CFLAG_NO_NAME_HASH))
                {
                    if((pbRootPtr + (sizeof(ULONGLONG) * RootGroup.Header.NumberOfFiles)) > pbRootEnd)
                        return NULL;
                    RootGroup.pHashes = (PULONGLONG)pbRootPtr;
                    pbRootPtr = pbRootPtr + (sizeof(ULONGLONG) * RootGroup.Header.NumberOfFiles);
                }

                return pbRootPtr;

            case RootFormatWoW_v1:
                if((pbRootPtr + (sizeof(FILE_ROOT_ENTRY) * RootGroup.Header.NumberOfFiles)) > pbRootEnd)
                    return NULL;
                RootGroup.pRootEntries = (PFILE_ROOT_ENTRY)pbRootPtr;

                // Return the position of the next block
                return pbRootPtr + (sizeof(FILE_ROOT_ENTRY) * RootGroup.Header.NumberOfFiles);

            default:
                return NULL;
        }
    }

    // Since WoW build 30080 (8.2.0)
    DWORD ParseWowRootFile_AddFiles_v2(TCascStorage * hs, FILE_ROOT_GROUP & RootGroup)
    {
        PCASC_CKEY_ENTRY pCKeyEntry;
        PCONTENT_KEY pCKey = RootGroup.pCKeyEntries;
        DWORD FileDataId = 0;

        // Sanity check
        assert(RootGroup.pCKeyEntries != NULL);

        // WoW.exe (build 19116): Blocks with zero files are skipped
        for(DWORD i = 0; i < RootGroup.Header.NumberOfFiles; i++, pCKey++)
        {
            // Set the file data ID
            FileDataId = FileDataId + RootGroup.FileDataIds[i];

            // Find the item in the central storage. Insert it to the tree
            if((pCKeyEntry = FindCKeyEntry_CKey(hs, pCKey->Value)) != NULL)
            {
                // If we know the file name hash, we're gonna insert it by hash AND file data id.
                // If we don't know the hash, we're gonna insert it just by file data id.
                if(RootGroup.pHashes != NULL && RootGroup.pHashes[i] != 0)
                {
                    FileTree.InsertByHash(pCKeyEntry, RootGroup.pHashes[i], FileDataId, RootGroup.Header.LocaleFlags, RootGroup.Header.ContentFlags);
                }
                else
                {
                    FileTree.InsertById(pCKeyEntry, FileDataId, RootGroup.Header.LocaleFlags, RootGroup.Header.ContentFlags);
                }
            }

            // Update the file data ID
            assert((FileDataId + 1) > FileDataId);
            FileDataId++;
        }

        return ERROR_SUCCESS;
    }

    // Since WoW build 18125 (6.0.1)
    DWORD ParseWowRootFile_AddFiles_v1(TCascStorage * hs, FILE_ROOT_GROUP & RootGroup)
    {
        PFILE_ROOT_ENTRY pRootEntry = RootGroup.pRootEntries;
        PCASC_CKEY_ENTRY pCKeyEntry;
        DWORD FileDataId = 0;

        // Sanity check
        assert(RootGroup.pRootEntries != NULL);

        // WoW.exe (build 19116): Blocks with zero files are skipped
        for(DWORD i = 0; i < RootGroup.Header.NumberOfFiles; i++, pRootEntry++)
        {
            // Set the file data ID
            FileDataId = FileDataId + RootGroup.FileDataIds[i];
//          BREAKIF(FileDataId == 2823765);

            // Find the item in the central storage. Insert it to the tree
            if((pCKeyEntry = FindCKeyEntry_CKey(hs, pRootEntry->CKey.Value)) != NULL)
            {
                if(pRootEntry->FileNameHash != 0)
                {
                    FileTree.InsertByHash(pCKeyEntry, pRootEntry->FileNameHash, FileDataId, RootGroup.Header.LocaleFlags, RootGroup.Header.ContentFlags);
                }
                else
                {
                    FileTree.InsertById(pCKeyEntry, FileDataId, RootGroup.Header.LocaleFlags, RootGroup.Header.ContentFlags);
                }
            }

            // Update the file data ID
            assert((FileDataId + 1) > FileDataId);
            FileDataId++;
        }

        return ERROR_SUCCESS;
    }

    DWORD ParseWowRootFile_Level2(
        TCascStorage * hs,
        LPBYTE pbRootPtr,
        LPBYTE pbRootEnd,
        DWORD dwLocaleMask,
        BYTE bOverrideLowViolence,
        BYTE bAudioLocale,
        DWORD dwRootVersion)
    {
        FILE_ROOT_GROUP RootBlock;

        // Reset the total file counter
        FileCounter = 0;

        // Now parse the root file
        while(pbRootPtr < pbRootEnd)
        {
            //char szMessage[0x100];
            //StringCchPrintfA(szMessage, _countof(szMessage), "%p\n", (pbRootEnd - pbRootPtr));
            //OutputDebugStringA(szMessage);

            // Validate the file locale block
            pbRootPtr = CaptureRootGroup(RootBlock, pbRootPtr, pbRootEnd, dwRootVersion);
            if(pbRootPtr == NULL)
                return ERROR_BAD_FORMAT;

            // WoW.exe (build 19116): Entries with flag 0x100 set are skipped
            if(RootBlock.Header.ContentFlags & CASC_CFLAG_DONT_LOAD)
                continue;

            // WoW.exe (build 19116): Entries with flag 0x80 set are skipped if overrideArchive CVAR is set to FALSE (which is by default in non-chinese clients)
            if((RootBlock.Header.ContentFlags & CASC_CFLAG_LOW_VIOLENCE) && bOverrideLowViolence == 0)
                continue;

            // WoW.exe (build 19116): Entries with (flags >> 0x1F) not equal to bAudioLocale are skipped
            if((RootBlock.Header.ContentFlags >> 0x1F) != bAudioLocale)
                continue;

            // WoW.exe (build 19116): Locales other than defined mask are skipped too
            if(RootBlock.Header.LocaleFlags != 0 && (RootBlock.Header.LocaleFlags & dwLocaleMask) == 0)
                continue;

            // Now call the custom function
            switch(RootFormat)
            {
                case RootFormatWoW_v2:
                    ParseWowRootFile_AddFiles_v2(hs, RootBlock);
                    break;

                case RootFormatWoW_v1:
                    ParseWowRootFile_AddFiles_v1(hs, RootBlock);
                    break;

                default:
                    return ERROR_NOT_SUPPORTED;
            }
        }

        return ERROR_SUCCESS;
    }

    /*
    #define CASC_LOCALE_BIT_ENUS            0x01
    #define CASC_LOCALE_BIT_KOKR            0x02
    #define CASC_LOCALE_BIT_RESERVED        0x03
    #define CASC_LOCALE_BIT_FRFR            0x04
    #define CASC_LOCALE_BIT_DEDE            0x05
    #define CASC_LOCALE_BIT_ZHCN            0x06
    #define CASC_LOCALE_BIT_ESES            0x07
    #define CASC_LOCALE_BIT_ZHTW            0x08
    #define CASC_LOCALE_BIT_ENGB            0x09
    #define CASC_LOCALE_BIT_ENCN            0x0A
    #define CASC_LOCALE_BIT_ENTW            0x0B
    #define CASC_LOCALE_BIT_ESMX            0x0C
    #define CASC_LOCALE_BIT_RURU            0x0D
    #define CASC_LOCALE_BIT_PTBR            0x0E
    #define CASC_LOCALE_BIT_ITIT            0x0F
    #define CASC_LOCALE_BIT_PTPT            0x10

        // dwLocale is obtained from a WOW_LOCALE_* to CASC_LOCALE_BIT_* mapping (sub_6615D0 in 7.0.3.22210 x86 win)
        // because (ENUS, ENGB) and (PTBR, PTPT) pairs share the same value on WOW_LOCALE_* enum
        // dwRegion is used to distinguish them
        if(dwRegion == WOW_REGION_EU)
        {
            // Is this english version of WoW?
            if(dwLocale == CASC_LOCALE_BIT_ENUS)
            {
                LoadWowRootFileLocales(hs, pbRootPtr, cbRootFile, CASC_LOCALE_ENGB, bOverrideArchive, bAudioLocale);
                LoadWowRootFileLocales(hs, pbRootPtr, cbRootFile, CASC_LOCALE_ENUS, bOverrideArchive, bAudioLocale);
                return ERROR_SUCCESS;
            }

            // Is this portuguese version of WoW?
            if(dwLocale == CASC_LOCALE_BIT_PTBR)
            {
                LoadWowRootFileLocales(hs, pbRootPtr, cbRootFile, CASC_LOCALE_PTPT, bOverrideArchive, bAudioLocale);
                LoadWowRootFileLocales(hs, pbRootPtr, cbRootFile, CASC_LOCALE_PTBR, bOverrideArchive, bAudioLocale);
            }
        }
        else
            LoadWowRootFileLocales(hs, pbRootPtr, cbRootFile, (1 << dwLocale), bOverrideArchive, bAudioLocale);
    */

    DWORD ParseWowRootFile_Level1(
        TCascStorage * hs,
        LPBYTE pbRootPtr,
        LPBYTE pbRootEnd,
        DWORD dwLocaleMask,
        BYTE bAudioLocale,
        DWORD dwRootVersion)
    {
        DWORD dwErrCode;

        // Load the locale as-is
        dwErrCode = ParseWowRootFile_Level2(hs, pbRootPtr, pbRootEnd, dwLocaleMask, false, bAudioLocale, dwRootVersion);
        if(dwErrCode != ERROR_SUCCESS)
            return dwErrCode;

        // If we wanted enGB, we also load enUS for the missing files
        if(dwLocaleMask == CASC_LOCALE_ENGB)
            ParseWowRootFile_Level2(hs, pbRootPtr, pbRootEnd, CASC_LOCALE_ENUS, false, bAudioLocale, dwRootVersion);

        if(dwLocaleMask == CASC_LOCALE_PTPT)
            ParseWowRootFile_Level2(hs, pbRootPtr, pbRootEnd, CASC_LOCALE_PTBR, false, bAudioLocale, dwRootVersion);

        return ERROR_SUCCESS;
    }

    // WoW.exe: 004146C7 (BuildManifest::Load)
    DWORD Load(TCascStorage * hs, LPBYTE pbRootPtr, LPBYTE pbRootEnd, DWORD dwLocaleMask, DWORD dwRootVersion)
    {
        DWORD dwErrCode;

        dwErrCode = ParseWowRootFile_Level1(hs, pbRootPtr, pbRootEnd, dwLocaleMask, 0, dwRootVersion);
        if(dwErrCode == ERROR_SUCCESS)
            dwErrCode = ParseWowRootFile_Level1(hs, pbRootPtr, pbRootEnd, dwLocaleMask, 1, dwRootVersion);

#ifdef CASCLIB_DEBUG
        // Dump the array of the file data IDs
        //FileTree.DumpFileDataIds("e:\\file-data-ids.bin");
#endif
        return dwErrCode;
    }

    // Search for files
    PCASC_CKEY_ENTRY Search(TCascSearch * pSearch, PCASC_FIND_DATA pFindData)
    {
        // If we have a listfile, we'll feed the listfile entries to the file tree
        if(pSearch->pCache != NULL && pSearch->bListFileUsed == false)
        {
            PCASC_FILE_NODE pFileNode;
            ULONGLONG FileNameHash;
            size_t nLength;
            DWORD FileDataId = CASC_INVALID_ID;
            char szFileName[MAX_PATH];

            if(RootFormat == RootFormatWoW_v2)
            {
                // Keep going through the listfile
                for(;;)
                {
                    // Retrieve the next line from the list file. Ignore lines that are too long to fit in the buffer
                    nLength = ListFile_GetNext(pSearch->pCache, szFileName, _countof(szFileName), &FileDataId);
                    if(nLength == 0)
                    {
                        if(GetCascError() == ERROR_INSUFFICIENT_BUFFER)
                            continue;
                        break;
                    }

                    // Try to verify the file name by hash
                    VerifyAndLogFileName(szFileName, FileDataId);

                    //
                    // Several files were renamed around WoW build 50893 (10.1.7). Example:
                    //
                    //  * 2965132; interface/icons/inv_helm_armor_explorer_d_01.blp     file name hash = 0x770b8d2dc4d940aa
                    //  * 2965132; interface/icons/inv_armor_explorer_d_01_helm.blp     file name hash = 0xf47ec17f4a1e49a2
                    //
                    // For that reason, we also need to check whether the file name hash matches
                    //

                    // BREAKIF(FileDataId == 2965132);

                    if((pFileNode = FileTree.FindById(FileDataId)) != NULL)
                    {
                        if(pFileNode->NameLength == 0)
                        {
                            if(pFileNode->FileNameHash && pFileNode->FileNameHash != CalcFileNameHash(szFileName))
                                continue;
                            FileTree.SetNodeFileName(pFileNode, szFileName);
                        }
                    }
                }
            }
            else
            {
                // Keep going through the listfile
                for(;;)
                {
                    // Retrieve the next line from the list file. Ignore lines that are too long to fit in the buffer
                    nLength = ListFile_GetNextLine(pSearch->pCache, szFileName, _countof(szFileName));
                    if(nLength == 0)
                    {
                        if(GetCascError() == ERROR_INSUFFICIENT_BUFFER)
                            continue;
                        break;
                    }

                    // Try to verify the file name by hash
                    VerifyAndLogFileName(szFileName, 0);

                    // Calculate the hash of the file name and lookup in tree
                    FileNameHash = CalcFileNameHash(szFileName);
                    pFileNode = FileTree.Find(FileNameHash);
                    if(pFileNode != NULL && pFileNode->NameLength == 0)
                    {
                        FileTree.SetNodeFileName(pFileNode, szFileName);
                    }
                }
            }
            pSearch->bListFileUsed = true;
        }

        // Let the file tree root give us the file names
        return TFileTreeRoot::Search(pSearch, pFindData);
    }

    ROOT_FORMAT RootFormat;                 // Root file format
    FILE * fp;                              // Handle to the dump file
    DWORD FileCounterHashless;              // Number of files for which we don't have hash. Meaningless for WoW before 8.2.0
    DWORD FileCounter;                      // Counter of loaded files. Only used during loading of ROOT file
};

//-----------------------------------------------------------------------------
// Public functions

DWORD RootHandler_CreateWoW(TCascStorage * hs, CASC_BLOB & RootFile, DWORD dwLocaleMask)
{
    TRootHandler_WoW * pRootHandler = NULL;
    ROOT_FORMAT RootFormat = RootFormatWoW_v1;
    LPCTSTR szDumpFile = NULL;
    LPBYTE pbRootFile = RootFile.pbData;
    LPBYTE pbRootEnd = RootFile.End();
    LPBYTE pbRootPtr;
    DWORD FileCounterHashless = 0;
    DWORD RootVersion = 0;
    DWORD dwErrCode = ERROR_BAD_FORMAT;

    // Verify the root header
    if((pbRootPtr = TRootHandler_WoW::CaptureRootHeader(pbRootFile, pbRootEnd, &RootFormat, &FileCounterHashless, &RootVersion)) == NULL)
        return ERROR_BAD_FORMAT;

#ifdef CASCLIB_WRITE_VERIFIED_FILENAMES
    LPCTSTR szExtension = (RootFormat == RootFormatWoW_v1) ? _T("txt") : _T("csv");
    TCHAR szBuffer[MAX_PATH];

    CascStrPrintf(szBuffer, _countof(szBuffer), _T("\\listfile_wow_%u_%s.%s"), hs->dwBuildNumber, hs->szCodeName, szExtension);
    szDumpFile = szBuffer;
#endif

    // Create the root handler
    pRootHandler = new TRootHandler_WoW(RootFormat, FileCounterHashless, szDumpFile);
    if(pRootHandler != NULL)
    {
        //fp = fopen("E:\\file-data-ids2.txt", "wt");

        // Load the root directory. If load failed, we free the object
        dwErrCode = pRootHandler->Load(hs, pbRootPtr, pbRootEnd, dwLocaleMask, RootVersion);
        if(dwErrCode != ERROR_SUCCESS)
        {
            delete pRootHandler;
            pRootHandler = NULL;
        }

        //fclose(fp);
    }

    // Assign the root directory (or NULL) and return error
    hs->pRootHandler = pRootHandler;
    return dwErrCode;
}

