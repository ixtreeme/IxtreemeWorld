package com.ixtreeme.client;

import android.app.NativeActivity;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.inputmethod.InputMethodManager;

public final class SoftKeyboardHelper {
    private SoftKeyboardHelper() {
    }

    public static void showSoftKeyboard(final NativeActivity activity) {
        if (activity == null) {
            return;
        }

        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                View view = activity instanceof IxtreemeNativeActivity
                    ? ((IxtreemeNativeActivity) activity).getInputBridgeView()
                    : activity.getWindow().getDecorView();
                if (view == null) {
                    return;
                }

                view.setFocusable(true);
                view.setFocusableInTouchMode(true);
                view.requestFocus();

                InputMethodManager imm = (InputMethodManager)
                    activity.getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.showSoftInput(view, InputMethodManager.SHOW_FORCED);
                }
            }
        });
    }

    public static void hideSoftKeyboard(final NativeActivity activity) {
        if (activity == null) {
            return;
        }

        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                View view = activity instanceof IxtreemeNativeActivity
                    ? ((IxtreemeNativeActivity) activity).getInputBridgeView()
                    : activity.getWindow().getDecorView();
                InputMethodManager imm = (InputMethodManager)
                    activity.getSystemService(Context.INPUT_METHOD_SERVICE);
                if (imm != null && view != null) {
                    imm.hideSoftInputFromWindow(view.getWindowToken(), 0);
                }
            }
        });
    }
}
