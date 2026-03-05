import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk
import subprocess
import threading
import os
import re
import json
from pathlib import Path

CONFIG_FILE = Path.home() / ".matrika_ide_config.json"

class MatrikaIDE:
    def __init__(self, root):
        self.root = root
        self.root.title("মাতৃকা IDE (Matrika)")
        self.root.geometry("1000x700")

        self.current_file = None
        self.modified = False
        self.config = self.load_config()
        self.exe_path = self.config.get("interpreter", "matrika.exe" if os.name == 'nt' else "./matrika")

        # Create the editor first so menus can reference it
        self.setup_editor_and_terminal()
        # Then build the menus (they need self.editor)
        self.setup_menus()
        # Finally the toolbar and status bar
        self.setup_toolbar_and_status()

        self.setup_tags()
        self.bind_events()
        self.update_status()

    # ---------- Configuration ----------
    def load_config(self):
        if CONFIG_FILE.exists():
            try:
                with open(CONFIG_FILE, "r", encoding="utf-8") as f:
                    return json.load(f)
            except:
                return {}
        return {}

    def save_config(self):
        with open(CONFIG_FILE, "w", encoding="utf-8") as f:
            json.dump(self.config, f, indent=2)

    # ---------- UI Setup ----------
    def setup_editor_and_terminal(self):
        # Main Paned Window (Editor + Terminal)
        self.paned = ttk.PanedWindow(self.root, orient=tk.VERTICAL)
        self.paned.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Editor Frame with line numbers
        editor_frame = ttk.Frame(self.paned)
        self.paned.add(editor_frame, weight=3)

        # Line numbers as a disabled Text widget
        self.line_numbers = tk.Text(editor_frame, width=4, bg='#f0f0f0', fg='#555',
                                    font=("Siyam Rupali", 14), state=tk.DISABLED,
                                    highlightthickness=0, borderwidth=0)
        self.line_numbers.pack(side=tk.LEFT, fill=tk.Y)

        # Text editor
        self.editor = tk.Text(editor_frame, font=("Siyam Rupali", 14), undo=True, wrap=tk.NONE,
                              yscrollcommand=self.on_editor_scroll)
        self.editor.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        # Scrollbar for editor
        scroll_y = ttk.Scrollbar(editor_frame, command=self.on_scrollbar_move)
        scroll_y.pack(side=tk.RIGHT, fill=tk.Y)
        self.editor.config(yscrollcommand=self.on_editor_scroll)

        # Terminal Frame
        terminal_frame = ttk.Frame(self.paned)
        self.paned.add(terminal_frame, weight=1)

        header = ttk.Frame(terminal_frame)
        header.pack(fill=tk.X)
        ttk.Label(header, text="টার্মিনাল (Terminal Output)", font=("Arial", 10, "bold")).pack(side=tk.LEFT)
        ttk.Button(header, text="মুছুন (Clear)", command=self.clear_terminal).pack(side=tk.RIGHT)

        self.terminal = tk.Text(terminal_frame, height=8, font=("Consolas", 12), bg="#1e1e1e", fg="#ffffff",
                                state=tk.DISABLED, wrap=tk.WORD)
        self.terminal.pack(fill=tk.BOTH, expand=True)

    def setup_menus(self):
        menubar = tk.Menu(self.root)
        self.root.config(menu=menubar)

        # File menu
        file_menu = tk.Menu(menubar, tearoff=0)
        file_menu.add_command(label="নতুন (New)", command=self.new_file, accelerator="Ctrl+N")
        file_menu.add_command(label="খুলুন (Open)", command=self.open_file, accelerator="Ctrl+O")
        file_menu.add_command(label="সংরক্ষণ (Save)", command=self.save_file, accelerator="Ctrl+S")
        file_menu.add_command(label="অন্য নামে সংরক্ষণ (Save As)", command=self.save_as_file)
        file_menu.add_separator()
        file_menu.add_command(label="প্রস্থান (Exit)", command=self.quit_app, accelerator="Ctrl+Q")
        menubar.add_cascade(label="ফাইল (File)", menu=file_menu)

        # Edit menu (now safe because self.editor exists)
        edit_menu = tk.Menu(menubar, tearoff=0)
        edit_menu.add_command(label="পূর্বাবস্থা (Undo)", command=self.editor.edit_undo, accelerator="Ctrl+Z")
        edit_menu.add_command(label="পুনরাবৃত্তি (Redo)", command=self.editor.edit_redo, accelerator="Ctrl+Y")
        edit_menu.add_separator()
        edit_menu.add_command(label="খুঁজুন (Find)", command=self.find_dialog, accelerator="Ctrl+F")
        edit_menu.add_command(label="প্রতিস্থাপন (Replace)", command=self.replace_dialog, accelerator="Ctrl+H")
        menubar.add_cascade(label="সম্পাদনা (Edit)", menu=edit_menu)

        # Run menu
        run_menu = tk.Menu(menubar, tearoff=0)
        run_menu.add_command(label="চালান (Run)", command=self.run_code, accelerator="F5")
        menubar.add_cascade(label="রান (Run)", menu=run_menu)

        # Snippets menu
        insert_menu = tk.Menu(menubar, tearoff=0)
        insert_menu.add_command(label="চলক (Variable)", command=lambda: self.insert_snippet('ধরি নাম = "পৃথিবী"\n'))
        insert_menu.add_command(label="ছাপানো (Print)", command=lambda: self.insert_snippet('বল("হ্যালো " + নাম)\n'))
        insert_menu.add_command(label="শর্ত (If)", command=lambda: self.insert_snippet('যদি (শর্ত) {\n    \n}\n'))
        insert_menu.add_command(label="লুপ (Loop)", command=lambda: self.insert_snippet('যতক্ষণ (শর্ত) {\n    \n}\n'))
        menubar.add_cascade(label="সাজেশন (Snippets)", menu=insert_menu)

        # Settings menu
        settings_menu = tk.Menu(menubar, tearoff=0)
        settings_menu.add_command(label="ইন্টারপ্রেটার পাথ সেট করুন (Set Interpreter Path)", command=self.set_interpreter_path)
        menubar.add_cascade(label="সেটিংস (Settings)", menu=settings_menu)

    def setup_toolbar_and_status(self):
        # Toolbar
        toolbar = ttk.Frame(self.root)
        toolbar.pack(side=tk.TOP, fill=tk.X)

        ttk.Button(toolbar, text="নতুন", command=self.new_file).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="খুলুন", command=self.open_file).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="সংরক্ষণ", command=self.save_file).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="রান", command=self.run_code).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="খুঁজুন", command=self.find_dialog).pack(side=tk.LEFT, padx=2)
        ttk.Button(toolbar, text="টার্মিনাল মুছুন", command=self.clear_terminal).pack(side=tk.LEFT, padx=2)

        # Status Bar
        self.status = ttk.Label(self.root, text=" প্রস্তুত (Ready)", relief=tk.SUNKEN, anchor=tk.W)
        self.status.pack(side=tk.BOTTOM, fill=tk.X)

    def setup_tags(self):
        self.editor.tag_configure("Keyword", foreground="#0000FF", font=("Siyam Rupali", 14, "bold"))
        self.editor.tag_configure("Function", foreground="#800080", font=("Siyam Rupali", 14, "bold"))
        self.editor.tag_configure("String", foreground="#008000")
        self.editor.tag_configure("Comment", foreground="#808080", font=("Siyam Rupali", 14, "italic"))
        self.editor.tag_configure("ErrorLine", background="#ffcccc", underline=True)
        self.editor.tag_configure("BracketMatch", background="#ffff99")

    def bind_events(self):
        self.root.bind("<F5>", lambda e: self.run_code())
        self.root.bind("<Control-n>", lambda e: self.new_file())
        self.root.bind("<Control-o>", lambda e: self.open_file())
        self.root.bind("<Control-s>", lambda e: self.save_file())
        self.root.bind("<Control-q>", lambda e: self.quit_app())
        self.root.bind("<Control-f>", lambda e: self.find_dialog())
        self.root.bind("<Control-h>", lambda e: self.replace_dialog())
        self.terminal.bind("<Button-1>", self.on_terminal_click)
        self.editor.bind("<<Modified>>", self.on_modified)
        self.editor.bind("<KeyRelease>", self.on_key_release)
        self.editor.bind("<Return>", self.auto_indent)
        self.editor.bind("<ButtonRelease-1>", self.update_status)

    # ---------- Line Numbers ----------
    def on_editor_scroll(self, *args):
        self.editor.yview(*args)
        self.line_numbers.yview(*args)
        self.update_line_numbers()

    def on_scrollbar_move(self, *args):
        self.editor.yview(*args)
        self.line_numbers.yview(*args)
        self.update_line_numbers()

    def update_line_numbers(self):
        # Enable line_numbers to modify it
        self.line_numbers.config(state=tk.NORMAL)
        self.line_numbers.delete("1.0", tk.END)

        # Get the first visible line index
        first = self.editor.index("@0,0")
        last = self.editor.index(f"@0,{self.editor.winfo_height()}")
        if not first or not last:
            self.line_numbers.config(state=tk.DISABLED)
            return

        start_line = int(first.split('.')[0])
        end_line = int(last.split('.')[0])

        for line in range(start_line, end_line + 1):
            self.line_numbers.insert(tk.END, f"{line:4d}\n")

        self.line_numbers.config(state=tk.DISABLED)

    # ---------- Editor Event Handlers ----------
    def on_modified(self, event=None):
        if self.editor.edit_modified():
            self.modified = True
            self.update_status()
            self.root.after_idle(self.highlight_syntax)
            self.editor.edit_modified(False)

    def on_key_release(self, event=None):
        self.update_status()
        self.match_brackets()

    def update_status(self, event=None):
        cursor = self.editor.index(tk.INSERT)
        line, col = cursor.split('.')
        modified = "*" if self.modified else ""
        fname = os.path.basename(self.current_file) if self.current_file else "নতুন ফাইল"
        self.status.config(text=f" লাইন: {line}  কলাম: {col}   ফাইল: {fname}{modified}")

    # ---------- Syntax Highlighting ----------
    def highlight_syntax(self):
        # Remove old tags
        for tag in ["Keyword", "Function", "String", "Comment", "ErrorLine"]:
            self.editor.tag_remove(tag, "1.0", tk.END)

        text = self.editor.get("1.0", tk.END)

        # Keywords
        keywords = ["ধরি", "বল", "যদি", "নতুবা", "যতক্ষণ", "ফেরত", "সত্য", "মিথ্যা", "মন্তব্য", "বিদায়"]
        for kw in keywords:
            for match in re.finditer(r'\b' + kw + r'\b', text):
                start = f"1.0 + {match.start()}c"
                end = f"1.0 + {match.end()}c"
                self.editor.tag_add("Keyword", start, end)

        # Function calls (like বল(...))
        for match in re.finditer(r'\b(বল|পড়ি)\s*\(', text):
            start = f"1.0 + {match.start()}c"
            end = f"1.0 + {match.end()-1}c"  # exclude '('
            self.editor.tag_add("Function", start, end)

        # Strings
        for match in re.finditer(r'".*?"', text):
            start = f"1.0 + {match.start()}c"
            end = f"1.0 + {match.end()}c"
            self.editor.tag_add("String", start, end)

        # Comments (both # and মন্তব্য)
        for match in re.finditer(r'(#|মন্তব্য).*', text):
            start = f"1.0 + {match.start()}c"
            end = f"1.0 + {match.end()}c"
            self.editor.tag_add("Comment", start, end)

    # ---------- Bracket Matching ----------
    def match_brackets(self):
        self.editor.tag_remove("BracketMatch", "1.0", tk.END)
        pos = self.editor.index(tk.INSERT)
        char = self.editor.get(pos)
        if char in "()[]{}":
            open_bracket = char
            close_bracket = {'(':')', '[':']', '{':'}'}[char]
            if char in "([{":
                start = self.editor.index(pos + "+1c")
                match = self.editor.search(close_bracket, start, stopindex=tk.END, regexp=False)
            else:
                start = self.editor.index(pos + "-1c")
                match = self.editor.search(open_bracket, start, stopindex="1.0", backwards=True, regexp=False)
            if match:
                self.editor.tag_add("BracketMatch", pos, pos + "+1c")
                self.editor.tag_add("BracketMatch", match, match + "+1c")

    # ---------- Auto-indent ----------
    def auto_indent(self, event):
        line = self.editor.get("insert linestart", "insert")
        indent = re.match(r'^(\s*)', line).group(1)
        self.editor.insert("insert", "\n" + indent)
        return "break"

    # ---------- Snippets ----------
    def insert_snippet(self, text):
        self.editor.insert(tk.INSERT, text)
        self.highlight_syntax()

    # ---------- Find & Replace ----------
    def find_dialog(self):
        self._find_replace_dialog(find_only=True)

    def replace_dialog(self):
        self._find_replace_dialog(find_only=False)

    def _find_replace_dialog(self, find_only):
        dialog = tk.Toplevel(self.root)
        dialog.title("খুঁজুন" if find_only else "প্রতিস্থাপন")
        dialog.geometry("400x150")
        dialog.transient(self.root)
        dialog.grab_set()

        ttk.Label(dialog, text="খুঁজুন:").grid(row=0, column=0, padx=5, pady=5, sticky=tk.W)
        find_entry = ttk.Entry(dialog, width=40)
        find_entry.grid(row=0, column=1, padx=5, pady=5)
        find_entry.focus()

        replace_entry = None
        if not find_only:
            ttk.Label(dialog, text="প্রতিস্থাপন:").grid(row=1, column=0, padx=5, pady=5, sticky=tk.W)
            replace_entry = ttk.Entry(dialog, width=40)
            replace_entry.grid(row=1, column=1, padx=5, pady=5)

        def find():
            pattern = find_entry.get()
            if not pattern:
                return
            self.editor.tag_remove("sel", "1.0", tk.END)
            pos = self.editor.search(pattern, "insert", stopindex=tk.END)
            if pos:
                self.editor.mark_set("insert", pos)
                self.editor.see(pos)
                self.editor.tag_add("sel", pos, f"{pos}+{len(pattern)}c")
            else:
                messagebox.showinfo("ফলাফল", "আর কিছু পাওয়া যায়নি।")

        def replace():
            if not replace_entry:
                return
            find_text = find_entry.get()
            replace_text = replace_entry.get()
            if self.editor.tag_ranges("sel"):
                start, end = self.editor.tag_ranges("sel")[0], self.editor.tag_ranges("sel")[1]
                if self.editor.get(start, end) == find_text:
                    self.editor.delete(start, end)
                    self.editor.insert(start, replace_text)
            find()

        def replace_all():
            if not replace_entry:
                return
            find_text = find_entry.get()
            replace_text = replace_entry.get()
            content = self.editor.get("1.0", tk.END)
            new_content = content.replace(find_text, replace_text)
            self.editor.delete("1.0", tk.END)
            self.editor.insert("1.0", new_content)

        btn_frame = ttk.Frame(dialog)
        btn_frame.grid(row=2, column=0, columnspan=2, pady=10)
        ttk.Button(btn_frame, text="পরবর্তী খুঁজুন (Find Next)", command=find).pack(side=tk.LEFT, padx=5)
        if not find_only:
            ttk.Button(btn_frame, text="প্রতিস্থাপন (Replace)", command=replace).pack(side=tk.LEFT, padx=5)
            ttk.Button(btn_frame, text="সব প্রতিস্থাপন (Replace All)", command=replace_all).pack(side=tk.LEFT, padx=5)

    # ---------- Terminal ----------
    def write_terminal(self, text):
        self.terminal.config(state=tk.NORMAL)
        self.terminal.insert(tk.END, text)
        self.terminal.see(tk.END)
        self.terminal.config(state=tk.DISABLED)

    def clear_terminal(self):
        self.terminal.config(state=tk.NORMAL)
        self.terminal.delete("1.0", tk.END)
        self.terminal.config(state=tk.DISABLED)

    def on_terminal_click(self, event):
        index = self.terminal.index(f"@{event.x},{event.y}")
        line = self.terminal.get(index + " linestart", index + " lineend")
        match = re.search(r'\[লাইন (\d+)\]', line)
        if match:
            line_num = int(match.group(1))
            self.editor.see(f"{line_num}.0")
            self.editor.mark_set("insert", f"{line_num}.0")
            self.editor.focus()

    # ---------- Run Code ----------
    def run_code(self):
        if not os.path.exists(self.exe_path):
            messagebox.showerror("ত্রুটি (Error)", f"ইন্টারপ্রেটার পাওয়া যায়নি! দয়া করে পাথ সেট করুন।\nবর্তমান পাথ: {self.exe_path}")
            return

        code = self.editor.get("1.0", tk.END).strip()
        if not code:
            return

        temp_file = "temp.matrika"
        with open(temp_file, "w", encoding="utf-8") as f:
            f.write(code)

        self.clear_terminal()
        self.editor.tag_remove("ErrorLine", "1.0", tk.END)

        threading.Thread(target=self.execute_subprocess, args=(temp_file,), daemon=True).start()

    def execute_subprocess(self, filename):
        try:
            process = subprocess.Popen(
                [self.exe_path, filename],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8"
            )
            stdout, stderr = process.communicate()

            if stdout:
                self.write_terminal(stdout)
            if stderr:
                self.write_terminal(stderr)
                self.trace_error(stderr)
            self.write_terminal("\n>>> এক্সিকিউশন শেষ (Execution Finished)\n")
        except Exception as e:
            self.write_terminal(f"\n[System Error] {str(e)}\n")

    def trace_error(self, stderr):
        match = re.search(r'\[লাইন (\d+)\]', stderr)
        if match:
            line_num = match.group(1)
            start_idx = f"{line_num}.0"
            end_idx = f"{line_num}.end"
            self.editor.tag_add("ErrorLine", start_idx, end_idx)

    # ---------- File Operations ----------
    def new_file(self):
        if self.modified:
            if not messagebox.askyesno("সংরক্ষণ করবেন?", "ফাইলটি সংরক্ষণ করা হয়নি। কি নতুন ফাইল খুলবেন?"):
                return
        self.editor.delete("1.0", tk.END)
        self.current_file = None
        self.modified = False
        self.root.title("মাতৃকা IDE - নতুন ফাইল")
        self.update_status()

    def open_file(self):
        if self.modified:
            if not messagebox.askyesno("সংরক্ষণ করবেন?", "ফাইলটি সংরক্ষণ করা হয়নি। কি খুলবেন?"):
                return
        filepath = filedialog.askopenfilename(filetypes=[("Matrika Files", "*.matrika"), ("All Files", "*.*")])
        if filepath:
            with open(filepath, "r", encoding="utf-8") as f:
                self.editor.delete("1.0", tk.END)
                self.editor.insert(tk.END, f.read())
            self.current_file = filepath
            self.modified = False
            self.root.title(f"মাতৃকা IDE - {os.path.basename(filepath)}")
            self.highlight_syntax()
            self.update_status()

    def save_file(self):
        if not self.current_file:
            self.save_as_file()
        else:
            with open(self.current_file, "w", encoding="utf-8") as f:
                f.write(self.editor.get("1.0", tk.END))
            self.modified = False
            self.root.title(f"মাতৃকা IDE - {os.path.basename(self.current_file)}")
            self.update_status()

    def save_as_file(self):
        filepath = filedialog.asksaveasfilename(defaultextension=".matrika", filetypes=[("Matrika Files", "*.matrika")])
        if filepath:
            self.current_file = filepath
            self.save_file()

    def quit_app(self):
        if self.modified:
            if not messagebox.askyesno("সংরক্ষণ করবেন?", "ফাইলটি সংরক্ষণ করা হয়নি। কি প্রস্থান করবেন?"):
                return
        self.root.quit()

    # ---------- Settings ----------
    def set_interpreter_path(self):
        path = filedialog.askopenfilename(title="মাতৃকা ইন্টারপ্রেটার নির্বাচন করুন")
        if path:
            self.exe_path = path
            self.config["interpreter"] = path
            self.save_config()
            messagebox.showinfo("সেটিংস", "ইন্টারপ্রেটার পাথ সংরক্ষিত হয়েছে।")

if __name__ == "__main__":
    root = tk.Tk()
    app = MatrikaIDE(root)
    root.mainloop()