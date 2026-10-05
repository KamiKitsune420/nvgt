package com.samtupy.nvgt;

import android.app.Activity;
import android.app.Dialog;
import android.os.Build;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.concurrent.ConcurrentLinkedQueue;

/**
 * Forms made of the phone's own controls, for a script to put on screen in place of an audio form, so that the screen reader reads them and the on-screen keyboard types into them like in any other app.
 * A script drives this with messages and hears back with events, both lists of fields separated by the unit separator character (31). It is what include/native_form.nvgt talks to; the messages are documented there.
 * Every form is a full screen dialog over the game holding one column of controls, each known by the index the script gave it.
 */
public final class NativeForm {
	private static final String SEP = "\u001f";
	private static final int FLAG_ENABLED = 1, FLAG_VISIBLE = 2, FLAG_READ_ONLY = 4, FLAG_MULTILINE = 8, FLAG_PASSWORD = 16;
	private static final ConcurrentLinkedQueue<String> events = new ConcurrentLinkedQueue<>();
	private static final HashMap<Integer, Form> forms = new HashMap<>(); // Only touched on the UI thread.

	private static final class Control {
		String type = "";
		int flags = FLAG_ENABLED | FLAG_VISIBLE;
		LinearLayout row; // Everything this control puts on screen.
		TextView label; // The caption above an input box, a list or a slider.
		View widget; // The part that takes focus.
		ArrayList<TextView> items = new ArrayList<>();
		int selected = -1;
		double min = 0, max = 100, step = 1;
		boolean quiet; // True while the script's own change is being applied, which is not something to tell it about.
	}

	private static final class Form {
		int id;
		Dialog dialog;
		LinearLayout column;
		ArrayList<Control> controls = new ArrayList<>();
		long alive; // When the script last said anything about this form.
	}

	// A script has no way to say that a form is finished with: it just stops looking at it. So a form that has not been heard from for a while is taken off the screen, and comes back if the script returns to it.
	private static final long ALIVE_TIME = 700;
	private static final android.os.Handler watchdog = new android.os.Handler(android.os.Looper.getMainLooper());
	private static boolean watching = false;
	private static void watch() {
		if (watching) return;
		watching = true;
		watchdog.postDelayed(new Runnable() {
			@Override public void run() {
				long now = android.os.SystemClock.uptimeMillis();
				for (Form form : forms.values()) {
					if (now - form.alive > ALIVE_TIME && form.dialog.isShowing()) form.dialog.hide();
				}
				if (forms.isEmpty()) watching = false;
				else watchdog.postDelayed(this, 200);
			}
		}, 200);
	}

	public static boolean available() {
		return true;
	}

	/** The next thing the user did, or null when there is nothing waiting. */
	public static byte[] receive() {
		String e = events.poll();
		return e != null ? e.getBytes(StandardCharsets.UTF_8) : null;
	}

	public static boolean send(final Activity activity, byte[] message) {
		if (activity == null || message == null) return false;
		final String[] f = new String(message, StandardCharsets.UTF_8).split(SEP, -1);
		if (f.length < 2) return false;
		activity.runOnUiThread(() -> {
			try {
				handle(activity, f);
			} catch (Exception e) {
				android.util.Log.e("NativeForm", "message " + f[0] + " failed", e);
			}
		});
		return true;
	}

	private static void event(String... fields) {
		events.add(String.join(SEP, fields));
	}

	private static int dp(Activity a, int value) {
		return (int)(value * a.getResources().getDisplayMetrics().density);
	}

	private static void handle(Activity activity, String[] f) {
		String op = f[0];
		int id = Integer.parseInt(f[1]);
		if (op.equals("open")) {
			open(activity, id, f.length > 2 ? f[2] : "");
			return;
		}
		Form form = forms.get(id);
		if (form == null) return;
		if (op.equals("close")) {
			forms.remove(id);
			form.dialog.dismiss();
			return;
		}
		form.alive = android.os.SystemClock.uptimeMillis();
		if (!form.dialog.isShowing()) form.dialog.show();
		if (op.equals("alive")) return;
		if (op.equals("title")) {
			form.dialog.setTitle(f[2]);
			return;
		}
		int index = Integer.parseInt(f[2]);
		if (op.equals("count")) {
			while (form.controls.size() > index) {
				Control c = form.controls.remove(form.controls.size() - 1);
				form.column.removeView(c.row);
			}
			return;
		}
		if (op.equals("control")) {
			while (form.controls.size() <= index) {
				Control filler = new Control();
				filler.row = new LinearLayout(activity);
				filler.row.setOrientation(LinearLayout.VERTICAL);
				form.controls.add(filler);
				form.column.addView(filler.row, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
			}
			build(activity, form, index, f[3], Integer.parseInt(f[4]), f[5]);
			return;
		}
		if (index < 0 || index >= form.controls.size()) return;
		Control c = form.controls.get(index);
		c.quiet = true;
		try {
			if (op.equals("label")) setLabel(c, f[3]);
			else if (op.equals("state")) setFlags(c, Integer.parseInt(f[3]));
			else if (op.equals("text")) setText(c, f[3]);
			else if (op.equals("check")) {
				if (c.widget instanceof CheckBox) ((CheckBox)c.widget).setChecked(f[3].equals("1"));
			} else if (op.equals("items")) setItems(activity, form, index, c, f);
			else if (op.equals("select")) select(c, Integer.parseInt(f[3]));
			else if (op.equals("range")) setRange(c, Double.parseDouble(f[3]), Double.parseDouble(f[4]), Double.parseDouble(f[5]), Double.parseDouble(f[6]));
			else if (op.equals("progress")) {
				if (c.widget instanceof ProgressBar) ((ProgressBar)c.widget).setProgress(Integer.parseInt(f[3]));
			} else if (op.equals("focus")) focus(c);
		} finally {
			c.quiet = false;
		}
	}

	private static void open(Activity activity, final int id, String title) {
		Form old = forms.remove(id);
		if (old != null) old.dialog.dismiss();
		final Form form = new Form();
		form.id = id;
		form.dialog = new Dialog(activity, android.R.style.Theme_DeviceDefault_NoActionBar);
		form.dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
		form.dialog.setTitle(title); // Read by the screen reader as the form opens.
		form.dialog.setCancelable(false);
		form.dialog.setOnKeyListener((d, keyCode, e) -> {
			if (keyCode != KeyEvent.KEYCODE_BACK && keyCode != KeyEvent.KEYCODE_ESCAPE) return false;
			if (e.getAction() == KeyEvent.ACTION_UP) event("cancel", String.valueOf(id));
			return true;
		});
		ScrollView scroller = new ScrollView(activity);
		scroller.setFillViewport(true);
		scroller.setFitsSystemWindows(true); // Keeps the controls out from under the status bar and the on-screen keyboard.
		form.column = new LinearLayout(activity);
		form.column.setOrientation(LinearLayout.VERTICAL);
		int pad = dp(activity, 16);
		form.column.setPadding(pad, pad, pad, pad);
		scroller.addView(form.column, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
		form.dialog.setContentView(scroller);
		Window w = form.dialog.getWindow();
		if (w != null) {
			w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
			w.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE | WindowManager.LayoutParams.SOFT_INPUT_STATE_HIDDEN);
		}
		form.alive = android.os.SystemClock.uptimeMillis();
		forms.put(id, form);
		form.dialog.show();
		watch();
	}

	// Tells the script which control the user has moved to, whether by keyboard focus or by the screen reader's own cursor.
	private static void watchFocus(final Form form, final int index, View v) {
		v.setOnFocusChangeListener((view, has) -> {
			if (has) event("focus", String.valueOf(form.id), String.valueOf(index));
		});
		v.setAccessibilityDelegate(new View.AccessibilityDelegate() {
			@Override public void sendAccessibilityEvent(View host, int type) {
				super.sendAccessibilityEvent(host, type);
				if (type == AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED) event("focus", String.valueOf(form.id), String.valueOf(index));
			}
		});
	}

	private static TextView makeLabel(Activity activity, Control c, String text) {
		c.label = new TextView(activity);
		c.label.setText(text);
		c.label.setPadding(0, dp(activity, 12), 0, dp(activity, 4));
		c.row.addView(c.label, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
		return c.label;
	}

	private static void build(final Activity activity, final Form form, final int index, String type, int flags, String label) {
		final Control c = form.controls.get(index);
		c.row.removeAllViews();
		c.type = type;
		c.label = null;
		c.widget = null;
		c.items.clear();
		c.selected = -1;
		final String fid = String.valueOf(form.id), cid = String.valueOf(index);
		LinearLayout.LayoutParams wide = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
		if (type.equals("button")) {
			Button b = new Button(activity);
			b.setAllCaps(false);
			b.setText(label);
			b.setOnClickListener(v -> event("click", fid, cid));
			c.widget = b;
			c.row.addView(b, wide);
		} else if (type.equals("checkbox")) {
			CheckBox b = new CheckBox(activity);
			b.setText(label);
			b.setMinHeight(dp(activity, 48));
			b.setOnCheckedChangeListener((v, checked) -> {
				if (!c.quiet) event("check", fid, cid, checked ? "1" : "0");
			});
			c.widget = b;
			c.row.addView(b, wide);
		} else if (type.equals("input")) {
			makeLabel(activity, c, label);
			final EditText e = new EditText(activity);
			boolean multiline = (flags & FLAG_MULTILINE) != 0, read_only = (flags & FLAG_READ_ONLY) != 0;
			int input = InputType.TYPE_CLASS_TEXT;
			if ((flags & FLAG_PASSWORD) != 0) input |= InputType.TYPE_TEXT_VARIATION_PASSWORD;
			else if (multiline) input |= InputType.TYPE_TEXT_FLAG_MULTI_LINE | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES;
			e.setInputType(input);
			if (multiline) {
				e.setSingleLine(false);
				e.setGravity(Gravity.TOP | Gravity.START);
				e.setMinLines(read_only ? 1 : 3);
			} else e.setImeOptions(EditorInfo.IME_ACTION_DONE);
			if (read_only) {
				// Still an edit box to the screen reader, so it can be read by character, word and line, but nothing can be typed and no keyboard comes up.
				e.setKeyListener(null);
				e.setTextIsSelectable(true);
				e.setShowSoftInputOnFocus(false);
			}
			e.setHint(label);
			if (Build.VERSION.SDK_INT >= 17) c.label.setLabelFor(View.generateViewId());
			if (Build.VERSION.SDK_INT >= 17) e.setId(c.label.getLabelFor());
			e.addTextChangedListener(new TextWatcher() {
				@Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
				@Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
				@Override public void afterTextChanged(Editable s) {
					if (!c.quiet) event("text", fid, cid, s.toString());
				}
			});
			e.setOnEditorActionListener((v, action, key) -> {
				if (action != EditorInfo.IME_ACTION_DONE && !(key != null && key.getKeyCode() == KeyEvent.KEYCODE_ENTER && key.getAction() == KeyEvent.ACTION_DOWN)) return false;
				event("submit", fid, cid);
				return true;
			});
			c.widget = e;
			c.row.addView(e, wide);
		} else if (type.equals("list")) {
			makeLabel(activity, c, label);
			if (Build.VERSION.SDK_INT >= 28) c.label.setAccessibilityHeading(true);
			// The items are added to the row itself by setItems.
		} else if (type.equals("slider")) {
			makeLabel(activity, c, label);
			SeekBar s = new SeekBar(activity);
			s.setContentDescription(label);
			s.setMinimumHeight(dp(activity, 48));
			s.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
				@Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
					if (!c.quiet) event("value", fid, cid, String.valueOf(c.min + progress * c.step));
				}
				@Override public void onStartTrackingTouch(SeekBar bar) {}
				@Override public void onStopTrackingTouch(SeekBar bar) {}
			});
			c.widget = s;
			c.row.addView(s, wide);
		} else if (type.equals("progress")) {
			makeLabel(activity, c, label);
			ProgressBar p = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
			p.setMax(100);
			p.setContentDescription(label);
			c.widget = p;
			c.row.addView(p, wide);
		} else if (type.equals("text")) {
			// A status bar: a caption and a line of text read together.
			TextView t = new TextView(activity);
			t.setText(label);
			t.setFocusable(true);
			t.setPadding(0, dp(activity, 12), 0, dp(activity, 12));
			c.widget = t;
			c.row.addView(t, wide);
		}
		if (c.widget != null) watchFocus(form, index, c.widget);
		c.quiet = true;
		setFlags(c, flags);
		c.quiet = false;
	}

	private static void setLabel(Control c, String label) {
		if (c.label != null) c.label.setText(label);
		if (c.widget instanceof EditText) ((EditText)c.widget).setHint(label);
		else if (c.widget instanceof SeekBar || c.widget instanceof ProgressBar) c.widget.setContentDescription(label);
		else if (c.widget instanceof TextView && c.label == null) ((TextView)c.widget).setText(label); // Button, CheckBox and status text.
	}

	private static void setFlags(Control c, int flags) {
		c.flags = flags;
		c.row.setVisibility((flags & FLAG_VISIBLE) != 0 ? View.VISIBLE : View.GONE);
		boolean enabled = (flags & FLAG_ENABLED) != 0;
		if (c.widget != null) c.widget.setEnabled(enabled && !(c.widget instanceof CheckBox && (flags & FLAG_READ_ONLY) != 0));
		for (TextView item : c.items) item.setEnabled(enabled);
	}

	private static void setText(Control c, String text) {
		if (c.widget instanceof EditText) {
			EditText e = (EditText)c.widget;
			if (e.getText().toString().equals(text)) return; // Usually the user's own typing coming back, which must not move their cursor.
			e.setText(text);
			if ((c.flags & FLAG_READ_ONLY) == 0) e.setSelection(e.getText().length());
		} else if (c.widget instanceof SeekBar) {
			if (Build.VERSION.SDK_INT >= 30) c.widget.setStateDescription(text.isEmpty() ? null : text); // What the slider's value is called, if it has a name.
		} else if (c.widget instanceof TextView) ((TextView)c.widget).setText(text);
	}

	private static void setRange(Control c, double min, double max, double step, double value) {
		if (!(c.widget instanceof SeekBar)) return;
		SeekBar s = (SeekBar)c.widget;
		if (step <= 0) step = 1;
		c.min = min;
		c.max = max;
		c.step = step;
		s.setMax((int)Math.max(0, Math.round((max - min) / step)));
		s.setProgress((int)Math.round((value - min) / step));
	}

	// Items are changed in place where they can be, so that the screen reader's cursor stays on the item it was reading when a list only grows or has a line reworded.
	private static void setItems(final Activity activity, final Form form, final int index, final Control c, String[] f) {
		int count = f.length - 4;
		while (c.items.size() > count) c.row.removeView(c.items.remove(c.items.size() - 1));
		for (int i = 0; i < count; i++) {
			String text = f[4 + i];
			if (i < c.items.size()) {
				if (!c.items.get(i).getText().toString().equals(text)) c.items.get(i).setText(text);
				continue;
			}
			final int position = i;
			TextView item = new TextView(activity);
			item.setText(text);
			item.setMinHeight(dp(activity, 48));
			item.setGravity(Gravity.CENTER_VERTICAL);
			if (c.label != null) item.setTextColor(c.label.getCurrentTextColor()); // One colour whatever its state: the theme's own for a selected item is too dark to see.
			item.setFocusable(true);
			item.setClickable(true);
			item.setEnabled((c.flags & FLAG_ENABLED) != 0);
			item.setOnClickListener(v -> {
				select(c, position);
				event("activate", String.valueOf(form.id), String.valueOf(index), String.valueOf(position));
			});
			// Moving the screen reader's cursor on to an item is what arrowing to it is in an audio form.
			item.setAccessibilityDelegate(new View.AccessibilityDelegate() {
				@Override public void sendAccessibilityEvent(View host, int type) {
					super.sendAccessibilityEvent(host, type);
					if (type != AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED) return;
					event("focus", String.valueOf(form.id), String.valueOf(index));
					if (c.selected == position) return;
					select(c, position);
					event("select", String.valueOf(form.id), String.valueOf(index), String.valueOf(position));
				}
			});
			c.items.add(item);
			c.row.addView(item, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
		}
		c.selected = -1;
		select(c, Integer.parseInt(f[3]));
	}

	private static void select(Control c, int position) {
		if (c.selected >= 0 && c.selected < c.items.size()) c.items.get(c.selected).setSelected(false);
		c.selected = position;
		if (position >= 0 && position < c.items.size()) c.items.get(position).setSelected(true);
	}

	private static void focus(Control c) {
		View v = c.widget;
		if (c.type.equals("list")) v = c.selected >= 0 && c.selected < c.items.size() ? c.items.get(c.selected) : c.items.isEmpty() ? c.label : c.items.get(0);
		if (v == null) return;
		final View target = v;
		// Posted, because a control that was only just added has not been laid out yet and cannot take the screen reader's cursor until it has.
		target.post(() -> {
			target.requestFocus();
			target.performAccessibilityAction(android.view.accessibility.AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS, null);
		});
	}
}
