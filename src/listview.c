////////////////////////////////////////////////////////////////////////////////
// UltraHLE - Ultra64 High Level Emulator
// Copyright (c) 1999, Epsilon and RealityMan
// THIS IS A PRIVATE NONPUBLIC VERSION. NOT FOR PUBLIC DISTRIBUTION!
// listview.c

#include "listview.h"
#include "zip/romzip.h"

   // Create Rom List View

   void CreateListView( void )
   {
      LV_COLUMN lvC;                   // List View Column Structure
      RECT rcl;                        // Window Size
      HIMAGELIST hSmall;               // List View Images
      HICON hIcon;                     // Icon Handle

      // Get the Size and Position of the Parent Window

      GetClientRect( hwndMain, &rcl );

      // Create the ListView Window

      hwndList = CreateWindowEx( 0L,
                                 WC_LISTVIEW,
                                 "",
                                 WS_VISIBLE | WS_CHILD | WS_BORDER |
                                 LVS_REPORT | WS_EX_CLIENTEDGE,
                                 0, 0,
                                 rcl.right - rcl.left,
                                 rcl.bottom - rcl.top - 99,
                                 hwndMain,
                                 (HMENU)ID_LISTVIEW,
                                 hInst,
                                 NULL );

      hwndDebug = CreateWindowEx( 0L,
                                  WC_LISTVIEW,
                                  "",
                                  WS_VISIBLE | WS_CHILD | WS_BORDER |
                                  LVS_REPORT | WS_EX_CLIENTEDGE,
                                  0,
                                  rcl.bottom - rcl.top - 100,
                                  rcl.right - rcl.left,
                                  80,
                                  hwndMain,
                                  (HMENU)ID_DEBUGVIEW,
                                  hInst,
                                  NULL );

      // Create the Image List

	   hSmall = ImageList_Create( 16, 16, TRUE, 6, 0 );

      // Load Image List Icons

      hIcon = LoadIcon( hInst, MAKEINTRESOURCE( IDI_GERMANY ) );
      ImageList_AddIcon( hSmall, hIcon );
      hIcon = LoadIcon( hInst, MAKEINTRESOURCE( IDI_USA ) );
      ImageList_AddIcon( hSmall, hIcon );
      hIcon = LoadIcon( hInst, MAKEINTRESOURCE( IDI_JAPAN ) );
      ImageList_AddIcon( hSmall, hIcon );
      hIcon = LoadIcon( hInst, MAKEINTRESOURCE( IDI_EUROPE ) );
      ImageList_AddIcon( hSmall, hIcon );
      hIcon = LoadIcon( hInst, MAKEINTRESOURCE( IDI_AUSTRALIA ) );
      ImageList_AddIcon( hSmall, hIcon );
      hIcon = LoadIcon( hInst, MAKEINTRESOURCE( IDI_N64CART ) );
      ImageList_AddIcon( hSmall, hIcon );

      // Associate the Image List with the List View Control

	   ListView_SetImageList( hwndList, hSmall, LVSIL_SMALL );
      
      // Create the ListView Columns

      lvC.mask = LVCF_FMT | LVCF_TEXT | LVCF_WIDTH;

      lvC.fmt = LVCFMT_LEFT;
      lvC.cx = rcl.right;
      lvC.pszText = "Debug Output";
      ListView_InsertColumn( hwndDebug, 0, &lvC );

      lvC.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;

      lvC.fmt = LVCFMT_LEFT;
      lvC.iSubItem = 0;
      lvC.cx = 175;
      lvC.pszText = "Name";
      ListView_InsertColumn( hwndList, 0, &lvC);

      lvC.fmt = LVCFMT_CENTER;
      lvC.iSubItem = 1;
      lvC.cx = 60;
      lvC.pszText = "Country";
      ListView_InsertColumn( hwndList, 1, &lvC);

      lvC.fmt = LVCFMT_CENTER;
      lvC.iSubItem = 2;
      lvC.cx = 60;
      lvC.pszText = "Size";
      ListView_InsertColumn( hwndList, 2, &lvC);

      lvC.fmt = LVCFMT_LEFT;
      lvC.iSubItem = 3;
      lvC.cx = 100;
      lvC.pszText = "Filename";
      ListView_InsertColumn( hwndList, 3, &lvC);

      lvC.fmt = LVCFMT_LEFT;
      lvC.iSubItem = 4;
      lvC.cx = 240;
      lvC.pszText = "Comments";
      ListView_InsertColumn( hwndList, 4, &lvC);
   }

   // Update the Available ROMs List

   BOOL UpdateROMList( void )
   {
      WIN32_FIND_DATA FileData;        // File Find Data Structure
      HANDLE hSearch;                  // File Search Handle
      int i, j;                        // Iterators
		HANDLE hFile;							// Handle to a File
      LV_ITEM lvI;                     // List View Item Structure
		char cHeader[64];						// ROM Header Information
		DWORD nBytesRead;	            	// Number of Bytes Read
		char temp[2];					   	// Temporary Characters
      LPCTSTR defString = "\0";        // Default String
      int romCount;

      // Known N64 ROM Image Extensions

      int filterCount = 7;
      LPCTSTR szFilter = "*.bin\0*.mov\0*.n64\0*.rom\0*.u64\0*.v64\0*.z64\0*.zip\0";

      // Initialise ROM Count

      romCount = 0;

      // Set Window Title

      wsprintf( szBuffer, "%s - Refreshing Available ROM List", APPNAME );
      SetWindowText( hwndMain, szBuffer );

      // Change to ROMs Directory

      // Get Available ROM Image Information

      for( i = 0; i <= filterCount; i++ )
      {
         // Find First Filename Matching Search Criteria

         // ST: insert path to filter
         char filter[MAX_PATH];
         strcpy(filter,init.rompath);
         strcat(filter,szFilter + (i * 6) );

         hSearch = FindFirstFile( filter , &FileData );

         if( hSearch != INVALID_HANDLE_VALUE )
         {
            while( GetLastError() != ERROR_NO_MORE_FILES )
            {
               // Open ROM File to get Header Information

                    // ST: insert path to filename
                    strcpy(romList.filename,init.rompath);
                    strcat(romList.filename,FileData.cFileName);

               if( romzip_iszip( romList.filename ) )
               {
                  // Zipped ROM: read just its header from the archive
                  int romSize = 0;
                  hFile = INVALID_HANDLE_VALUE;
                  if( romzip_peek( romList.filename, cHeader, 64, &romSize ) )
                  {
                     FindNextFile( hSearch, &FileData );
                     continue; // not a ROM archive
                  }
                  romList.dwFileSize = romSize;
               }
               else
               {
					hFile = CreateFile( romList.filename,
											  GENERIC_READ,
											  FILE_SHARE_READ,
											  NULL,
											  OPEN_EXISTING,
											  FILE_ATTRIBUTE_READONLY,
											  NULL);

               // Get File Size

               romList.dwFileSize = GetFileSize( hFile, NULL );

					// Read ROM Header

					ReadFile( hFile, cHeader, 64, &nBytesRead, NULL );
               }

					// Swap Header Bytes Accordingly

                    //cart_flipheader(cHeader); // ST:
					if( cHeader[0] == 0x37 )
						_swab( cHeader, cHeader, 64 );

					if( cHeader[0] == 0x40 )
					{
						_swab( cHeader, cHeader, 64 );
						for( j = 0; j < 16; j++ )
						{
							CopyMemory( temp, cHeader + (j*4), 2 );
							CopyMemory( cHeader + (j*4), cHeader + ((j*4)+2), 2 );
							CopyMemory( cHeader + ((j*4)+2), temp, 2 );
						}
					}

					// Fill RomList Structure

               ZeroMemory( romList.name, sizeof( romList.name ) );
					CopyMemory( romList.name, cHeader + 32, 20 );

					romList.country = cHeader[0x3E];

					if( hFile != INVALID_HANDLE_VALUE ) CloseHandle( hFile );

					// Add Item to List View

               lvI.mask = LVIF_IMAGE;
               lvI.iItem = romCount;
               lvI.iSubItem = 0;

					switch( romList.country )
					{
						case 0x44:
                     lvI.iImage = 0;
                     ListView_InsertItem( hwndList, &lvI );
							ListView_SetItemText( hwndList, romCount, 1, "GER" );
							break;
						case 0x45:
                     lvI.iImage = 1;
                     ListView_InsertItem( hwndList, &lvI );
							ListView_SetItemText( hwndList, romCount, 1, "USA" );
							break;
						case 0x4A:
                     lvI.iImage = 2;
                     ListView_InsertItem( hwndList, &lvI );
							ListView_SetItemText( hwndList, romCount, 1, "JAP" );
							break;
						case 0x50:
                     lvI.iImage = 3;
                     ListView_InsertItem( hwndList, &lvI );
							ListView_SetItemText( hwndList, romCount, 1, "EUR" );
							break;
						case 0x55:
                     lvI.iImage = 4;
                     ListView_InsertItem( hwndList, &lvI );
							ListView_SetItemText( hwndList, romCount, 1, "AUS" );
							break;
						default:
                     lvI.iImage = 5;
                     ListView_InsertItem( hwndList, &lvI );
							ListView_SetItemText( hwndList, romCount, 1, "???" );
							break;
					}

               romList.alttitle[0] = '\0';
               romList.comment[0] = '\0';
               inifile_readtemp( romList.name );
               ListView_SetItemText( hwndList, romCount, 4, romList.comment );

               if( strlen(romList.alttitle) )
               {
                  ListView_SetItemText( hwndList, romCount, 0, romList.alttitle );
               }
               else
               {
                  ListView_SetItemText( hwndList, romCount, 0, romList.name );
               }

               wsprintf( szBuffer, "%iMbit", romList.dwFileSize / 131072 );
               ListView_SetItemText( hwndList, romCount, 2, szBuffer );

               ListView_SetItemText( hwndList, romCount, 3, romList.filename );

					// Increment ROM Count

               romCount++;

               // Get Next Available ROM File

               FindNextFile( hSearch, &FileData );
            }
         }

         // Close this Search

         FindClose( hSearch );
      }

      // Set Window Title

      wsprintf( szBuffer, "%s - %s v%i.%i.%i",
                APPNAME, TITLE, MAJORREV, MINORREV, PATCHLVL );
      SetWindowText( hwndMain, szBuffer );

      wsprintf( szBuffer, "%s - %s v%i.%i.%i - %s",
                APPNAME, TITLE, MAJORREV, MINORREV, PATCHLVL, COPYRIGHT1 );
      AddDebugLine( szBuffer, 0 );
#if !RELEASE
      AddDebugLine( COPYRIGHT2, 1 );
#endif

      return( TRUE );
   }

   // Add One Line to Debug List View

   void AddDebugLine( char *szBuf, int lineno )
   {
      LV_ITEM lvI;                     // List View Item Structure
      static int line=0;

      // ST: changes
      // - text is inserted directly
      // - linenum is ignored (internal line used instead)
      // - If text is NULL listbox is cleared.

      if(!szBuf)
      {
          ListView_DeleteAllItems( hwndDebug );
          line=0;
          return;
      }

      lvI.mask = LVIF_TEXT; // | LVIF_IMAGE;
      lvI.iItem = line;
      lvI.iSubItem = 0;
      lvI.iImage = 0;
      lvI.pszText=szBuf;

      ListView_InsertItem( hwndDebug, &lvI );
      //ListView_SetItemText( hwndDebug, lineno, 0, szBuf );
      ListView_EnsureVisible( hwndDebug, line, FALSE);

      line++;
   }

   // Copies Debug Output lines to the clipboard as plain text, one per line:
   // the selected lines, or every line if none is selected (or all is set).
   void CopyDebugLines( int all )
   {
      int     n = ListView_GetItemCount( hwndDebug );
      int     sel = all ? 0 : ListView_GetSelectedCount( hwndDebug );
      int     i, pass;
      size_t  total = 0, l;
      char    text[1024];
      char   *p = NULL;
      HGLOBAL h = NULL;

      if( n <= 0 ) return;

      for( pass = 0; pass < 2; pass++ )
      {
         for( i = 0; i < n; i++ )
         {
            if( sel && !(ListView_GetItemState( hwndDebug, i, LVIS_SELECTED ) & LVIS_SELECTED) )
               continue;
            text[0] = 0;
            ListView_GetItemText( hwndDebug, i, 0, text, sizeof( text ) );
            l = strlen( text );
            if( pass )
            {
               memcpy( p, text, l );
               p += l;
               *p++ = '\r';
               *p++ = '\n';
            }
            else total += l + 2;
         }
         if( !pass )
         {
            h = GlobalAlloc( GMEM_MOVEABLE, total + 1 );
            if( !h ) return;
            p = (char *)GlobalLock( h );
            if( !p ) { GlobalFree( h ); return; }
         }
      }
      *p = 0;
      GlobalUnlock( h );

      if( OpenClipboard( hwndMain ) )
      {
         EmptyClipboard();
         if( !SetClipboardData( CF_TEXT, h ) ) GlobalFree( h ); // clipboard owns h on success
         CloseClipboard();
      }
      else GlobalFree( h );
   }

   void SelectAllDebugLines( void )
   {
      ListView_SetItemState( hwndDebug, -1, LVIS_SELECTED, LVIS_SELECTED );
   }

