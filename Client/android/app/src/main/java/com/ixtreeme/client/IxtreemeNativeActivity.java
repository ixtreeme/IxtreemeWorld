package com.ixtreeme.client;

import android.app.NativeActivity;
import android.os.Bundle;
import android.text.Editable;
import android.text.SpannableStringBuilder;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

public class IxtreemeNativeActivity extends NativeActivity {
    static {
        System.loadLibrary("VulkanClear");
    }

    public static native void nativeOnTextInput(String text);
    public static native void nativeOnKeyDown(int keyCode);
    public static native void nativeOnKeyUp(int keyCode);
    public static native void nativeOnDeleteBackward();

    private InputBridgeView inputBridgeView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ensureInputBridgeView();
    }

    public View getInputBridgeView() {
        return ensureInputBridgeView();
    }

    private View ensureInputBridgeView() {
        if (inputBridgeView != null) {
            return inputBridgeView;
        }

        inputBridgeView = new InputBridgeView(this);
        inputBridgeView.setFocusable(true);
        inputBridgeView.setFocusableInTouchMode(true);

        ViewGroup.LayoutParams params = new ViewGroup.LayoutParams(1, 1);
        addContentView(inputBridgeView, params);
        return inputBridgeView;
    }

    private static final class InputBridgeView extends View {
        private final SpannableStringBuilder editable = new SpannableStringBuilder();

        InputBridgeView(IxtreemeNativeActivity activity) {
            super(activity);
        }

        @Override
        public boolean onCheckIsTextEditor() {
            return true;
        }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
            outAttrs.inputType = InputType.TYPE_CLASS_TEXT |
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS |
                InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
            outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE |
                EditorInfo.IME_FLAG_NO_EXTRACT_UI;

            return new BaseInputConnection(this, true) {
                @Override
                public Editable getEditable() {
                    return editable;
                }

                @Override
                public boolean commitText(CharSequence text, int newCursorPosition) {
                    if (text != null && text.length() > 0) {
                        IxtreemeNativeActivity.nativeOnTextInput(text.toString());
                    }
                    editable.clear();
                    return true;
                }

                @Override
                public boolean sendKeyEvent(KeyEvent event) {
                    int keyCode = event.getKeyCode();
                    if (event.getAction() == KeyEvent.ACTION_DOWN) {
                        IxtreemeNativeActivity.nativeOnKeyDown(keyCode);
                    } else if (event.getAction() == KeyEvent.ACTION_UP) {
                        IxtreemeNativeActivity.nativeOnKeyUp(keyCode);
                    }
                    return true;
                }

                @Override
                public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                    int count = Math.max(1, beforeLength);
                    for (int i = 0; i < count; ++i) {
                        IxtreemeNativeActivity.nativeOnDeleteBackward();
                    }
                    editable.clear();
                    return true;
                }
            };
        }
    }
}
