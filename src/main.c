/*
Copyright (c) 2008-2011
	Lars-Dominik Braun <lars@6xq.net>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#define _POSIX_C_SOURCE 1 /* fileno() */
#define _BSD_SOURCE /* strdup() */

/* system includes */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/select.h>
#include <time.h>
#include <ctype.h>
/* open () */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
/* tcset/getattr () */
#include <termios.h>
#include <pthread.h>
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <signal.h>

/* pandora.com library */
#include <piano.h>

#include "main.h"
#include "terminal.h"
#include "config.h"
#include "ui.h"
#include "ui_dispatch.h"
#include "ui_readline.h"
#include "download.h"

/* Normalizing strdrup for paths, etc. */

bool _nchar( char c ){
    if ( 48 <= c && c <= 57 ) { /* 0 .. 9 */
        return true;
    }
    if ( 65 <= c && c <= 90 ) { /* A .. Z */
        return true;
    }
    else if ( 97 <= c && c <= 122 ) { /* a .. z */
        return true;
    }
    else if ( c == 95 ) { /* _ */
        return true;
    }
    return false;
}

char *_nstrdup( const char *s0 ){
    char *s1 = malloc( strlen( s0 ) + 1 );
    char *s1i = 0;
    memset( s1, 0, strlen( s0 ) + 1 );

    s1i = s1;
    while( *s0 ){

        if ( _nchar( *s0 ) ) {
            /* Normal character, A-Za-z_, just copy it */
            *s1i = *s0;
            s1i++;
        }
        else {
            /* Not a normal character, attempt to replace with _ ... */
            if ( s1i == s1 ) {
                /* At the beginning of the string, just skip */
            } 
            else if ( *(s1i - 1) == '_' ) {
                /* Already have a _, just skip */
            }
            else {
                *s1i = '_';
                s1i++;
            }
        }

        s0++;
    }

    if ( *(s1i - 1) == '_' ) {
        /* Strip trailing _ */
        *(s1i - 1) = 0;
    }

    return s1;
}

char *_slash2dash_strdup( const char *s0 ){
    char *s1 = strdup( s0 );
    char *s1i = s1;
    while ( s1i = strchr( s1i, '/' ) ) {
        *s1i = '-';
    }
    return s1;
}

static void BarDownloadFilename(BarApp_t *app) {
	char baseFilename[1024 * 2];
	char songFilename[1024 * 2];
    const char *separator = 0;
    PianoSong_t *song = app->playlist;
    PianoStation_t *station = app->curStation;
    BarDownload_t *download = &(app->player.download);

    memset(songFilename, 0, sizeof (songFilename));
    memset(baseFilename, 0, sizeof (baseFilename));

    separator = app->settings.downloadSeparator;

    {
	    char *artist = 0, *album = 0, *title = 0;

        if ( app->settings.downloadSafeFilename ){
            artist = _nstrdup(song->artist);
            album = _nstrdup(song->album);
            title = _nstrdup(song->title);
        }
        else {
            artist = _slash2dash_strdup(song->artist);
            album = _slash2dash_strdup(song->album);
            title = _slash2dash_strdup(song->title);
        }

        strcpy(songFilename, artist);
        strcat(songFilename, separator);
        strcat(songFilename, album);
        strcat(songFilename, separator);
        strcat(songFilename, title);

        free(artist);
        free(album);
        free(title);
    }

    switch (song->audioFormat) {
        #ifdef ENABLE_FAAD
        case PIANO_AF_AACPLUS:
            strcat(songFilename, ".aac");
            break;
        #endif
        #ifdef ENABLE_MAD
        case PIANO_AF_MP3_HI:
            strcat(songFilename, ".hifi");
        case PIANO_AF_MP3:
            strcat(songFilename, ".mp3");
            break;
        #endif
        default:
            strcat(songFilename, ".dump");
            break;
    }

    strcpy(baseFilename, app->settings.download);
    // TODO Check if trailing slash exists
	strcat(baseFilename, "/");
	mkdir(baseFilename, S_IRWXU | S_IRWXG);

    {
        char *station_ = 0;
        if ( app->settings.downloadSafeFilename ){
            station_ = _nstrdup( station->name );
        }
        else {
            station_ = _slash2dash_strdup( station->name );
        }
        strcat( baseFilename, station_ );
        free( station_ );
    }
	strcat(baseFilename, "/");
	mkdir(baseFilename, S_IRWXU | S_IRWXG);

    /* Loved filename */
    strcpy( download->lovedFilename, baseFilename );
    strcat( download->lovedFilename, songFilename );

    /* Unloved filename */
    strcpy( download->unlovedFilename, baseFilename );
    strcat( download->unlovedFilename, "/unloved/" );
	mkdir( download->unlovedFilename, S_IRWXU | S_IRWXG);
    strcat( download->unlovedFilename, songFilename );

    /* Downloading filename */
    strcpy( download->downloadingFilename, baseFilename );
    strcat( download->downloadingFilename, ".downloading-" );
    strcat( download->downloadingFilename, songFilename );
}

void BarDownloadStart(BarApp_t *app) {

    /* Indicate that the song is loved so we save it to the right place */
    app->player.download.loveSong = app->playlist->rating == PIANO_RATE_LOVE ? 1 : 0;

    /* Pass through the cleanup setting */
    app->player.download.cleanup = app->settings.downloadCleanup;

    if (! app->settings.download) {
        /* No download directory set, so return */
        return;
    }

    BarDownloadFilename(app);

    if (access(app->player.download.downloadingFilename, R_OK) != 0) {
        app->player.download.handle = fopen(app->player.download.downloadingFilename, "w");
    } else {
        app->player.download.handle = NULL;
    }

}

static void BarMainLoadProxy (const BarSettings_t *settings,
		WaitressHandle_t *waith) {
	char tmpPath[2];

	/* set up proxy (control proxy for non-us citizen or global proxy for poor
	 * firewalled fellows) */
	if (settings->controlProxy != NULL) {
		/* control proxy overrides global proxy */
		WaitressSplitUrl (settings->controlProxy, waith->proxyHost,
				sizeof (waith->proxyHost), waith->proxyPort,
				sizeof (waith->proxyPort), tmpPath, sizeof (tmpPath));
	} else if (settings->proxy != NULL && strlen (settings->proxy) > 0) {
		WaitressSplitUrl (settings->proxy, waith->proxyHost,
				sizeof (waith->proxyHost), waith->proxyPort,
				sizeof (waith->proxyPort), tmpPath, sizeof (tmpPath));
	}
}

/*	authenticate user
 */
static bool BarMainLoginUser (BarApp_t *app) {
	PianoReturn_t pRet;
	WaitressReturn_t wRet;
	PianoRequestDataLogin_t reqData;
	bool ret;

	reqData.user = app->settings.username;
	reqData.password = app->settings.password;
	reqData.step = 0;

	BarUiMsg (MSG_INFO, "Login... ");
	ret = BarUiPianoCall (app, PIANO_REQUEST_LOGIN, &reqData, &pRet, &wRet);
	BarUiStartEventCmd (&app->settings, "userlogin", NULL, NULL, &app->player,
			NULL, pRet, wRet);
	return ret;
}

/*	ask for username/password if none were provided in settings
 */
static void BarMainGetLoginCredentials (BarSettings_t *settings,
		BarReadlineFds_t *input) {
	if (settings->username == NULL) {
		char nameBuf[100];
		BarUiMsg (MSG_QUESTION, "Username: ");
		BarReadlineStr (nameBuf, sizeof (nameBuf), input, BAR_RL_DEFAULT);
		settings->username = strdup (nameBuf);
	}
	if (settings->password == NULL) {
		char passBuf[100];
		BarUiMsg (MSG_QUESTION, "Password: ");
		BarReadlineStr (passBuf, sizeof (passBuf), input, BAR_RL_NOECHO);
		write (STDIN_FILENO, "\n", 1);
		settings->password = strdup (passBuf);
	}
}

/*	get station list
 */
static bool BarMainGetStations (BarApp_t *app) {
	PianoReturn_t pRet;
	WaitressReturn_t wRet;
	bool ret;

	BarUiMsg (MSG_INFO, "Get stations... ");
	ret = BarUiPianoCall (app, PIANO_REQUEST_GET_STATIONS, NULL, &pRet, &wRet);
	BarUiStartEventCmd (&app->settings, "usergetstations", NULL, NULL, &app->player,
			app->ph.stations, pRet, wRet);
	return ret;
}

/*	get initial station from autostart setting or user input
 */
static void BarMainGetInitialStation (BarApp_t *app) {
	/* try to get autostart station */
	if (app->settings.autostartStation != NULL) {
		app->curStation = PianoFindStationById (app->ph.stations,
				app->settings.autostartStation);
		if (app->curStation == NULL) {
			BarUiMsg (MSG_ERR, "Error: Autostart station not found.\n");
		}
	}
	/* no autostart? ask the user */
	if (app->curStation == NULL) {
		app->curStation = BarUiSelectStation (&app->ph, "Select station: ",
				app->settings.sortOrder, &app->input);
	}
	if (app->curStation != NULL) {
		BarUiPrintStation (app->curStation);
	}
}

/*	wait for user input
 */
static void BarMainHandleUserInput (BarApp_t *app) {
	char buf[2];
	if (BarReadline (buf, sizeof (buf), NULL, &app->input,
			BAR_RL_FULLRETURN | BAR_RL_NOECHO, 1) > 0) {
		BarUiDispatch (app, buf[0], app->curStation, app->playlist, true,
				BAR_DC_GLOBAL);
    }
}


/*	fetch new playlist
 */
static void BarMainGetPlaylist (BarApp_t *app) {
	PianoReturn_t pRet;
	WaitressReturn_t wRet;
	PianoRequestDataGetPlaylist_t reqData;
	reqData.station = app->curStation;
	reqData.format = app->settings.audioFormat;

	BarUiMsg (MSG_INFO, "Receiving new playlist... ");
	if (!BarUiPianoCall (app, PIANO_REQUEST_GET_PLAYLIST,
			&reqData, &pRet, &wRet)) {
		app->curStation = NULL;
	} else {
		app->playlist = reqData.retPlaylist;
		if (app->playlist == NULL) {
			BarUiMsg (MSG_INFO, "No tracks left.\n");
			app->curStation = NULL;
		}
	}
	BarUiStartEventCmd (&app->settings, "stationfetchplaylist",
			app->curStation, app->playlist, &app->player, app->ph.stations,
			pRet, wRet);
}

/*	start new player thread
 */
static void BarMainStartPlayback (BarApp_t *app, pthread_t *playerThread) {
	BarUiPrintSong (&app->settings, app->playlist, app->curStation->isQuickMix ?
			PianoFindStationById (app->ph.stations,
			app->playlist->stationId) : NULL);

	if (app->playlist->audioUrl == NULL) {
		BarUiMsg (MSG_ERR, "Invalid song url.\n");
	} else {
		/* setup player */
		memset (&app->player, 0, sizeof (app->player));

		WaitressInit (&app->player.waith);
		WaitressSetUrl (&app->player.waith, app->playlist->audioUrl);

		/* set up global proxy, player is NULLed on songfinish */
		if (app->settings.proxy != NULL) {
			char tmpPath[2];
			WaitressSplitUrl (app->settings.proxy,
					app->player.waith.proxyHost,
					sizeof (app->player.waith.proxyHost),
					app->player.waith.proxyPort,
					sizeof (app->player.waith.proxyPort), tmpPath,
					sizeof (tmpPath));
		}

		app->player.gain = app->playlist->fileGain;
		app->player.scale = BarPlayerCalcScale (app->player.gain + app->settings.volume);
		app->player.audioFormat = app->playlist->audioFormat;

		/* throw event */
		BarUiStartEventCmd (&app->settings, "songstart",
				app->curStation, app->playlist, &app->player, app->ph.stations,
				PIANO_RET_OK, WAITRESS_RET_OK);

        BarDownloadStart(app);

		/* prevent race condition, mode must _not_ be FREED if
		 * thread has been started */
		app->player.mode = PLAYER_STARTING;
		/* start player */
		pthread_create (playerThread, NULL, BarPlayerThread,
				&app->player);
	}
}

/*	player is done, clean up
 */
static void BarMainPlayerCleanup (BarApp_t *app, pthread_t *playerThread) {
	void *threadRet;

	BarUiStartEventCmd (&app->settings, "songfinish", app->curStation,
			app->playlist, &app->player, app->ph.stations, PIANO_RET_OK,
			WAITRESS_RET_OK);

	/* FIXME: pthread_join blocks everything if network connection
	 * is hung up e.g. */
	pthread_join (*playerThread, &threadRet);

	/* don't continue playback if thread reports error */
	if (threadRet != (void *) PLAYER_RET_OK) {
		app->curStation = NULL;
	}

	memset (&app->player, 0, sizeof (app->player));
}

/*	print song duration
 */
static void BarMainPrintTime (BarApp_t *app) {
	/* Ugly: songDuration is unsigned _long_ int! Lets hope this won't
	 * overflow */
	int songRemaining = (signed long int) (app->player.songDuration -
			app->player.songPlayed) / BAR_PLAYER_MS_TO_S_FACTOR;
	enum {POSITIVE, NEGATIVE} sign = NEGATIVE;
	if (songRemaining < 0) {
		/* song is longer than expected */
		sign = POSITIVE;
		songRemaining = -songRemaining;
	}
	BarUiMsg (MSG_TIME, "%c%02i:%02i/%02i:%02i\r",
			(sign == POSITIVE ? '+' : '-'),
			songRemaining / 60, songRemaining % 60,
			app->player.songDuration / BAR_PLAYER_MS_TO_S_FACTOR / 60,
			app->player.songDuration / BAR_PLAYER_MS_TO_S_FACTOR % 60);
}

/*	main loop
 */
static void BarMainLoop (BarApp_t *app) {
	pthread_t playerThread;

	BarMainGetLoginCredentials (&app->settings, &app->input);

	BarMainLoadProxy (&app->settings, &app->waith);

	if (!BarMainLoginUser (app)) {
		return;
	}

	if (!BarMainGetStations (app)) {
		return;
	}

	BarMainGetInitialStation (app);

	/* little hack, needed to signal: hey! we need a playlist, but don't
	 * free anything (there is nothing to be freed yet) */
	memset (&app->player, 0, sizeof (app->player));

	while (!app->doQuit) {
		/* song finished playing, clean up things/scrobble song */
		if (app->player.mode == PLAYER_FINISHED_PLAYBACK) {
			BarMainPlayerCleanup (app, &playerThread);
		}

		/* check whether player finished playing and start playing new
		 * song */
		if (app->player.mode >= PLAYER_FINISHED_PLAYBACK ||
				app->player.mode == PLAYER_FREED) {
			if (app->curStation != NULL) {
				/* what's next? */
				if (app->playlist != NULL) {
					PianoSong_t *histsong = app->playlist;
					app->playlist = app->playlist->next;
					BarUiHistoryPrepend (app, histsong);
				}
				if (app->playlist == NULL) {
					BarMainGetPlaylist (app);
				}
				/* song ready to play */
				if (app->playlist != NULL) {
					BarMainStartPlayback (app, &playerThread);
				}
			}
		}

		BarMainHandleUserInput (app);

		/* show time */
		if (app->player.mode >= PLAYER_SAMPLESIZE_INITIALIZED &&
				app->player.mode < PLAYER_FINISHED_PLAYBACK) {
			BarMainPrintTime (app);
		}
	}

	if (app->player.mode != PLAYER_FREED) {
		pthread_join (playerThread, NULL);
	}
}

static BarApp_t *glapp = 0;

static void BarCleanup (int sig) {
    if ( glapp ) {
        if ( glapp->player.download.cleanup ) {
            unlink( glapp->player.download.downloadingFilename );
        }
    }
    signal( sig, SIG_DFL );
    raise( sig );
}

int main (int argc, char **argv) {
    static BarApp_t app;
	char ctlPath[PATH_MAX];
	/* terminal attributes _before_ we started messing around with ~ECHO */
	struct termios termOrig;

    glapp = &app;
	memset (&app, 0, sizeof (app));

	/* save terminal attributes, before disabling echoing */
	BarTermSave (&termOrig);
	BarTermSetEcho (0);
	BarTermSetBuffer (0);

	/* init some things */
	ao_initialize ();
	PianoInit (&app.ph);

	WaitressInit (&app.waith);
	strncpy (app.waith.host, PIANO_RPC_HOST, sizeof (app.waith.host)-1);
	strncpy (app.waith.port, PIANO_RPC_PORT, sizeof (app.waith.port)-1);

	BarSettingsInit (&app.settings);
	BarSettingsRead (&app.settings);

	BarUiMsg (MSG_NONE, "Welcome to " PACKAGE " (" VERSION ")! ");
	if (app.settings.keys[BAR_KS_HELP] == BAR_KS_DISABLED) {
		BarUiMsg (MSG_NONE, "\n");
	} else {
		BarUiMsg (MSG_NONE, "Press %c for a list of commands.\n",
				app.settings.keys[BAR_KS_HELP]);
	}

	/* init fds */
	FD_ZERO(&app.input.set);
	app.input.fds[0] = STDIN_FILENO;
	FD_SET(app.input.fds[0], &app.input.set);

	BarGetXdgConfigDir (PACKAGE "/ctl", ctlPath, sizeof (ctlPath));
	/* open fifo read/write so it won't EOF if nobody writes to it */
	assert (sizeof (app.input.fds) / sizeof (*app.input.fds) >= 2);
	app.input.fds[1] = open (ctlPath, O_RDWR);
	if (app.input.fds[1] != -1) {
		FD_SET(app.input.fds[1], &app.input.set);
		BarUiMsg (MSG_INFO, "Control fifo at %s opened\n", ctlPath);
	}
	app.input.maxfd = app.input.fds[0] > app.input.fds[1] ? app.input.fds[0] :
			app.input.fds[1];
	++app.input.maxfd;

    signal( SIGINT, BarCleanup );
    signal( SIGTERM, BarCleanup );
	BarMainLoop (&app);

	if (app.input.fds[1] != -1) {
		close (app.input.fds[1]);
	}

	PianoDestroy (&app.ph);
	PianoDestroyPlaylist (app.songHistory);
	PianoDestroyPlaylist (app.playlist);
	ao_shutdown();
	BarSettingsDestroy (&app.settings);

	/* restore terminal attributes, zsh doesn't need this, bash does... */
	BarTermRestore (&termOrig);

	return 0;
}

