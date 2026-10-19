package org.gameport.app;

import android.os.Bundle;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/** Hosts the recompiled game (libmain.so) through SDL. */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }
}
