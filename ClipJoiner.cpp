// ClipJoiner v0.3 - join two video clips (back to back or with a 0.5 s
// crossfade), or split one clip in two at a chosen time.
//
// Drop two clips into the two slots, press Join. The result is written to the
// "Save to" folder (remembered between runs) as <clip1>_joined.mp4, with _2,
// _3... added if that name is taken.
// The heavy lifting is done by ffmpeg.exe / ffprobe.exe (looked up next to
// ClipJoiner.exe first, then on PATH). No FFmpeg libraries are linked.
//
// Join strategy:
//   1. Both clips share codec / size / fps / pixel format / audio layout
//      -> concat demuxer with stream copy: instant, lossless.
//   2. Anything differs, the copy fails, or crossfade is on
//      -> concat filter re-encode (libx264 CRF 17 veryfast + AAC 192k),
//         clip 2 scaled/padded to clip 1's size and fps, silence added
//         for a clip that has no audio track.
//   Crossfade: xfade (video) + acrossfade (audio), 0.5 s overlap, so the
//   result is 0.5 s shorter than the two clips added together.

#include <wx/wx.h>
#include <wx/dnd.h>
#include <wx/filename.h>
#include <wx/process.h>
#include <wx/stdpaths.h>
#include <wx/file.h>
#include <wx/utils.h>
#include <wx/dcbuffer.h>
#include <wx/config.h>
#include <wx/dirdlg.h>
#include <wx/dcmemory.h>
#include <wx/statbmp.h>
#include <wx/bmpbndl.h>

#include <memory>
#include <vector>

// ---------------------------------------------------------------- palette --

namespace pal {
const wxColour Bg(0x0f, 0x13, 0x18);
const wxColour Slot(0x16, 0x1b, 0x22);
const wxColour SlotHover(0x1c, 0x24, 0x2e);
const wxColour Border(0x30, 0x36, 0x3d);
const wxColour Accent(0x4c, 0x9a, 0xff);
const wxColour Text(0xe6, 0xed, 0xf3);
const wxColour Dim(0x8b, 0x94, 0x9e);
const wxColour Warn(0xe3, 0xb3, 0x41);
const wxColour Err(0xf8, 0x51, 0x49);
const wxColour Ok(0x3f, 0xb9, 0x50);
}  // namespace pal

static wxFont MonoFont(int pt, bool bold = false) {
    wxFont f(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE).FaceName("Consolas"));
    if (!f.IsOk()) f = wxFont(wxFontInfo(pt).Family(wxFONTFAMILY_TELETYPE));
    if (bold) f.MakeBold();
    return f;
}

// ------------------------------------------------------------------ tools --

// Full quoted path to ffmpeg/ffprobe if it sits next to our exe, else the
// bare name so the OS searches PATH.
static wxString ToolCmd(const wxString& name) {
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
#ifdef __WXMSW__
    wxFileName local(exe.GetPath(), name + ".exe");
#else
    wxFileName local(exe.GetPath(), name);
#endif
    if (local.FileExists()) return "\"" + local.GetFullPath() + "\"";
    return name;
}

static wxString Quote(const wxString& p) { return "\"" + p + "\""; }

static const double kFadeSeconds = 0.5;

static int RunSync(const wxString& cmd, wxArrayString& out) {
    wxArrayString err;
    return (int)wxExecute(cmd, out, err, wxEXEC_SYNC | wxEXEC_NODISABLE | wxEXEC_HIDE_CONSOLE);
}

// --------------------------------------------------------------- clip info --

struct ClipInfo {
    wxString path;
    bool ok = false;
    wxString error;
    // video
    wxString vcodec, pixFmt, fpsRaw;
    long w = 0, h = 0;
    double fps = 0;
    // audio
    bool hasAudio = false;
    wxString acodec;
    long sampleRate = 0, channels = 0;
    // container
    double duration = 0;
    // video stream length (falls back to container duration); used for the fade point
    double vdur = 0;
};

static double ParseRate(const wxString& r) {
    double num = 0, den = 1;
    wxString a = r.BeforeFirst('/'), b = r.AfterFirst('/');
    a.ToCDouble(&num);
    if (!b.empty()) b.ToCDouble(&den);
    return den > 0 ? num / den : 0;
}

static ClipInfo ProbeClip(const wxString& path) {
    ClipInfo ci;
    ci.path = path;
    wxString cmd = ToolCmd("ffprobe") +
                   " -v error -show_entries "
                   "stream=codec_type,codec_name,width,height,r_frame_rate,pix_fmt,sample_rate,channels,duration"
                   ":format=duration -of compact " +
                   Quote(path);
    wxArrayString out;
    long rc = RunSync(cmd, out);
    if (rc == -1) {
        ci.error = "ffprobe could not be started (not next to ClipJoiner.exe or on PATH)";
        return ci;
    }
    bool haveVideo = false;
    for (const wxString& line : out) {
        wxArrayString f = wxSplit(line, '|', '\0');
        if (f.empty()) continue;
        wxString kind = f[0];
        auto get = [&](const wxString& key) -> wxString {
            for (size_t i = 1; i < f.size(); ++i)
                if (f[i].BeforeFirst('=') == key) return f[i].AfterFirst('=');
            return wxString();
        };
        if (kind == "stream") {
            wxString type = get("codec_type");
            if (type == "video" && !haveVideo) {
                haveVideo = true;
                ci.vcodec = get("codec_name");
                ci.pixFmt = get("pix_fmt");
                ci.fpsRaw = get("r_frame_rate");
                ci.fps = ParseRate(ci.fpsRaw);
                get("width").ToLong(&ci.w);
                get("height").ToLong(&ci.h);
                get("duration").ToCDouble(&ci.vdur);
            } else if (type == "audio" && !ci.hasAudio) {
                ci.hasAudio = true;
                ci.acodec = get("codec_name");
                get("sample_rate").ToLong(&ci.sampleRate);
                get("channels").ToLong(&ci.channels);
            }
        } else if (kind == "format") {
            get("duration").ToCDouble(&ci.duration);
        }
    }
    if (!haveVideo) {
        ci.error = rc == 0 ? wxString("no video stream found")
                           : wxString("ffprobe could not read this file");
        return ci;
    }
    if (ci.vdur <= 0) ci.vdur = ci.duration;
    ci.ok = true;
    return ci;
}

static wxString FormatTime(double s) {
    if (s < 0) s = 0;
    int m = (int)(s / 60);
    return wxString::Format("%d:%04.1f", m, s - m * 60);
}

static wxString Describe(const ClipInfo& c) {
    wxString fps = c.fps == (int)c.fps ? wxString::Format("%d", (int)c.fps)
                                       : wxString::Format("%.3f", c.fps);
    wxString s = wxString::Format("%ldx%ld  %s fps  %s  %s", c.w, c.h, fps,
                                  FormatTime(c.duration), c.vcodec);
    s += c.hasAudio ? wxString("+" + c.acodec) : wxString("  (no audio)");
    return s;
}

static bool StreamCopyCompatible(const ClipInfo& a, const ClipInfo& b, wxString& why) {
    if (a.vcodec != b.vcodec) why = "video codecs differ";
    else if (a.w != b.w || a.h != b.h) why = "resolutions differ";
    else if (a.fpsRaw != b.fpsRaw) why = "frame rates differ";
    else if (a.pixFmt != b.pixFmt) why = "pixel formats differ";
    else if (a.hasAudio != b.hasAudio) why = "only one clip has audio";
    else if (a.hasAudio && (a.acodec != b.acodec || a.sampleRate != b.sampleRate ||
                            a.channels != b.channels))
        why = "audio formats differ";
    else return true;
    return false;
}

// -------------------------------------------------------------- clip slot --

class ClipSlot;

class SlotDropTarget : public wxFileDropTarget {
public:
    explicit SlotDropTarget(ClipSlot* slot) : m_slot(slot) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& files) override;
    wxDragResult OnEnter(wxCoord, wxCoord, wxDragResult def) override;
    void OnLeave() override;

private:
    ClipSlot* m_slot;
};

wxDECLARE_EVENT(EVT_SLOT_FILES, wxCommandEvent);  // dropped/browsed files
wxDEFINE_EVENT(EVT_SLOT_FILES, wxCommandEvent);

class ClipSlot : public wxPanel {
public:
    ClipSlot(wxWindow* parent, int index) : wxPanel(parent, wxID_ANY), m_index(index) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(520, 96)));
        SetDropTarget(new SlotDropTarget(this));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &ClipSlot::OnPaint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { Browse(); });
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { SetHover(true); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { SetHover(false); });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
    }

    int Index() const { return m_index; }
    const ClipInfo& Info() const { return m_info; }
    bool HasClip() const { return m_info.ok; }

    void SetClip(const ClipInfo& ci) {
        m_info = ci;
        Refresh();
    }
    void Clear() {
        m_info = ClipInfo();
        Refresh();
    }
    void SetHover(bool h) {
        if (m_hover != h) {
            m_hover = h;
            Refresh();
        }
    }
    void SetEnabledLook(bool e) {
        m_enabled = e;
        SetCursor(wxCursor(e ? wxCURSOR_HAND : wxCURSOR_ARROW));
        Refresh();
    }
    bool IsEnabledLook() const { return m_enabled; }

    void PostFiles(const wxArrayString& files) {
        wxCommandEvent ev(EVT_SLOT_FILES, GetId());
        ev.SetInt(m_index);
        ev.SetString(wxJoin(files, '\n', '\0'));
        ev.SetEventObject(this);
        wxPostEvent(GetParent(), ev);
    }

private:
    void Browse() {
        if (!m_enabled) return;
        wxFileDialog dlg(this, wxString::Format("Choose clip %d", m_index + 1), "", "",
                         "Video files (*.mp4;*.mov;*.mkv;*.webm;*.avi)|*.mp4;*.mov;*.mkv;*.webm;*.avi|"
                         "All files (*.*)|*.*",
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            wxArrayString files;
            files.Add(dlg.GetPath());
            PostFiles(files);
        }
    }

    void OnPaint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        wxSize sz = GetClientSize();
        dc.SetBackground(wxBrush(pal::Bg));
        dc.Clear();

        bool filled = m_info.ok;
        wxColour border = m_hover && m_enabled ? pal::Accent : (filled ? pal::Border : pal::Border);
        wxPen pen(border, FromDIP(filled ? 1 : 2), filled ? wxPENSTYLE_SOLID : wxPENSTYLE_SHORT_DASH);
        dc.SetPen(pen);
        dc.SetBrush(wxBrush(m_hover && m_enabled ? pal::SlotHover : pal::Slot));
        int r = FromDIP(6);
        dc.DrawRoundedRectangle(1, 1, sz.x - 2, sz.y - 2, r);

        int pad = FromDIP(14);
        int x = pad, y = pad;
        dc.SetFont(MonoFont(9, true));
        dc.SetTextForeground(pal::Accent);
        wxString label = wxString::Format("CLIP %d", m_index + 1);
        dc.DrawText(label, x, y);
        int lh = dc.GetTextExtent(label).y;

        if (filled) {
            wxFileName fn(m_info.path);
            dc.SetFont(MonoFont(11, true));
            dc.SetTextForeground(pal::Text);
            dc.DrawText(Ellipsize(dc, fn.GetFullName(), sz.x - 2 * pad), x, y + lh + FromDIP(6));
            dc.SetFont(MonoFont(9));
            dc.SetTextForeground(pal::Dim);
            dc.DrawText(Ellipsize(dc, Describe(m_info), sz.x - 2 * pad), x,
                        y + lh + FromDIP(6) + dc.GetCharHeight() + FromDIP(10));
        } else {
            dc.SetFont(MonoFont(11));
            dc.SetTextForeground(m_info.error.empty() ? pal::Dim : pal::Err);
            wxString msg = m_info.error.empty() ? wxString("Drop a video here, or click to browse")
                                                : wxString("Couldn't read clip: " + m_info.error);
            dc.DrawText(Ellipsize(dc, msg, sz.x - 2 * pad), x, y + lh + FromDIP(10));
        }
    }

    static wxString Ellipsize(wxDC& dc, const wxString& s, int width) {
        return wxControl::Ellipsize(s, dc, wxELLIPSIZE_MIDDLE, width);
    }

    int m_index;
    ClipInfo m_info;
    bool m_hover = false;
    bool m_enabled = true;
};

bool SlotDropTarget::OnDropFiles(wxCoord, wxCoord, const wxArrayString& files) {
    m_slot->SetHover(false);
    if (!m_slot->IsEnabledLook()) return false;
    m_slot->PostFiles(files);
    return true;
}
wxDragResult SlotDropTarget::OnEnter(wxCoord, wxCoord, wxDragResult def) {
    m_slot->SetHover(true);
    return def;
}
void SlotDropTarget::OnLeave() { m_slot->SetHover(false); }

// ----------------------------------------------------------- split dialog --

static void RevealInFolder(const wxString& path) {
    if (!wxFileExists(path)) return;
#ifdef __WXMSW__
    wxExecute("explorer.exe /select,\"" + path + "\"", wxEXEC_ASYNC);
#else
    wxLaunchDefaultApplication(wxFileName(path).GetPath());
#endif
}

// "15", "15.5", "0:15.5", "1:02" -> seconds. Returns false if it can't parse.
static bool ParseTime(wxString s, double& out) {
    s.Trim(true).Trim(false);
    if (s.empty()) return false;
    double secs = 0, mins = 0;
    if (s.Contains(":")) {
        if (!s.BeforeFirst(':').ToCDouble(&mins) || !s.AfterFirst(':').ToCDouble(&secs)) return false;
        if (mins < 0 || secs < 0) return false;
        out = mins * 60 + secs;
        return true;
    }
    if (!s.ToCDouble(&secs)) return false;
    out = secs;
    return true;
}

// Pops up from the main window's "Split..." button. Splits one clip into
// <name>_part1.mp4 and <name>_part2.mp4 in the save folder, frame-exact.
class SplitDialog : public wxDialog {
public:
    SplitDialog(wxWindow* parent, const ClipInfo& clip, const wxString& saveDir)
        : wxDialog(parent, wxID_ANY, "Split clip", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE),
          m_clip(clip),
          m_saveDir(saveDir) {
#ifdef __WXMSW__
        SetIcon(wxICON(JOINY_ICON));
#endif
        SetBackgroundColour(pal::Bg);
        m_fps = clip.fps > 0 ? clip.fps : 30.0;
        m_frames = (long)(clip.vdur * m_fps + 0.5);

        m_previewSize = FromDIP(wxSize(120, 190));
        m_preview = new wxStaticBitmap(this, wxID_ANY, BlankPreview());

        auto* name = new wxStaticText(this, wxID_ANY, wxFileName(clip.path).GetFullName(),
                                      wxDefaultPosition, FromDIP(wxSize(380, -1)),
                                      wxST_ELLIPSIZE_MIDDLE);
        name->SetFont(MonoFont(10, true));
        name->SetForegroundColour(pal::Text);
        auto* info = new wxStaticText(this, wxID_ANY,
                                      wxString::Format("Length %s, %ld frames", FormatTime(clip.vdur),
                                                       m_frames));
        info->SetFont(MonoFont(9));
        info->SetForegroundColour(pal::Dim);

        auto* at = new wxStaticText(this, wxID_ANY, "Split at");
        at->SetFont(MonoFont(10));
        at->SetForegroundColour(pal::Text);
        double mid = clip.vdur / 2;
        m_time = new wxTextCtrl(this, wxID_ANY, wxString::Format("%.1f", mid), wxDefaultPosition,
                                FromDIP(wxSize(90, -1)), wxTE_PROCESS_ENTER);
        m_time->SetFont(MonoFont(10));
        auto* sec = new wxStaticText(this, wxID_ANY, "sec");
        sec->SetFont(MonoFont(9));
        sec->SetForegroundColour(pal::Dim);

        m_parts = new wxStaticText(this, wxID_ANY, "\n");
        m_parts->SetFont(MonoFont(9));
        m_parts->SetForegroundColour(pal::Dim);

        m_status = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                    wxST_ELLIPSIZE_MIDDLE);
        m_status->SetFont(MonoFont(9));

        m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, FromDIP(wxSize(-1, 6)));
        m_gauge->Hide();

        m_split = new wxButton(this, wxID_ANY, "Split");
        m_split->SetFont(MonoFont(10, true));
        m_reveal = new wxButton(this, wxID_ANY, "Show in folder");
        m_reveal->SetFont(MonoFont(10));
        m_reveal->Hide();
        m_close = new wxButton(this, wxID_CANCEL, "Close");
        m_close->SetFont(MonoFont(10));

        auto* timeRow = new wxBoxSizer(wxHORIZONTAL);
        timeRow->Add(at, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        timeRow->Add(m_time, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        timeRow->Add(sec, 0, wxALIGN_CENTER_VERTICAL);

        auto* right = new wxBoxSizer(wxVERTICAL);
        right->Add(name, 0, wxEXPAND);
        right->Add(info, 0, wxTOP, FromDIP(2));
        right->Add(timeRow, 0, wxTOP, FromDIP(16));
        right->Add(m_parts, 0, wxTOP, FromDIP(12));
        right->AddStretchSpacer();
        right->Add(m_gauge, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        right->Add(m_status, 0, wxEXPAND);

        auto* top = new wxBoxSizer(wxHORIZONTAL);
        top->Add(m_preview, 0, wxRIGHT, FromDIP(16));
        top->Add(right, 1, wxEXPAND);

        auto* btns = new wxBoxSizer(wxHORIZONTAL);
        btns->Add(m_reveal, 0);
        btns->AddStretchSpacer();
        btns->Add(m_close, 0, wxRIGHT, FromDIP(8));
        btns->Add(m_split, 0);

        auto* s = new wxBoxSizer(wxVERTICAL);
        s->Add(top, 1, wxEXPAND | wxALL, FromDIP(16));
        s->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        SetSizerAndFit(s);
        CentreOnParent();
        SetEscapeId(wxID_CANCEL);

        m_time->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            Validate();
            m_previewTimer.StartOnce(350);  // refresh the frame once typing pauses
        });
        m_time->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { StartSplit(); });
        m_split->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartSplit(); });
        m_reveal->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RevealInFolder(m_out1); });
        m_previewTimer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { UpdatePreview(); });
        m_pulse.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { m_gauge->Pulse(); });
        Bind(wxEVT_END_PROCESS, &SplitDialog::OnProcessEnd, this);
        Bind(wxEVT_CLOSE_WINDOW, &SplitDialog::OnClose, this);
        m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

        Validate();
        UpdatePreview();
        m_time->SetFocus();
        m_time->SelectAll();
    }

    ~SplitDialog() override {
        if (!m_previewFile.empty() && wxFileExists(m_previewFile)) wxRemoveFile(m_previewFile);
    }

private:
    // The cut sits halfway between frame n-1 and frame n, so part 1 ends on
    // frame n-1 and part 2 starts on frame n (the frame shown in the preview),
    // with no rounding trouble at the boundary.
    double CutPoint() const { return (m_cutFrame - 0.5) / m_fps; }
    double FrameTime() const { return m_cutFrame / m_fps; }

    bool Validate() {
        double t = 0;
        bool ok = ParseTime(m_time->GetValue(), t);
        wxString err;
        if (!ok) {
            err = "Enter a time like 15, 15.5 or 0:15.5";
        } else {
            long n = (long)(t * m_fps + 0.5);
            if (n < 1 || n > m_frames - 1)
                err = wxString::Format("Time must be inside the clip (0:00.0 to %s)",
                                       FormatTime(m_clip.vdur));
            else
                m_cutFrame = n;
        }
        m_valid = err.empty();
        if (m_valid) {
            m_parts->SetLabel(wxString::Format("-> part 1: 0:00.0 to %s  (%ld frames)\n"
                                               "-> part 2: %s to %s  (%ld frames)",
                                               FormatTime(FrameTime()), m_cutFrame,
                                               FormatTime(FrameTime()), FormatTime(m_clip.vdur),
                                               m_frames - m_cutFrame));
            if (!m_running) SetStatusText("", pal::Dim);
        } else {
            m_parts->SetLabel("\n");
            if (!m_running) SetStatusText(err, pal::Err);
        }
        m_split->Enable(m_valid && !m_running);
        Layout();
        return m_valid;
    }

    void SetStatusText(const wxString& t, const wxColour& c) {
        m_status->SetForegroundColour(c);
        m_status->SetLabel(t);
        m_status->Refresh();
    }

    wxBitmap BlankPreview() {
        wxBitmap bmp(m_previewSize);
        wxMemoryDC dc(bmp);
        dc.SetBackground(wxBrush(pal::Slot));
        dc.Clear();
        dc.SetPen(wxPen(pal::Border));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(wxPoint(0, 0), m_previewSize);
        return bmp;
    }

    void UpdatePreview() {
        if (!m_valid || m_running) return;
        if (m_previewFile.empty()) {
            m_previewFile = wxFileName::CreateTempFileName("clipjoiner_preview");
            if (m_previewFile.empty()) return;
            wxRemoveFile(m_previewFile);
            m_previewFile += ".png";
        }
        // Accurate seek: the first decoded frame at or after the cut point,
        // which is exactly the first frame of part 2.
        wxString cmd = ToolCmd("ffmpeg") +
                       wxString::Format(" -hide_banner -v error -y -ss %.6f -i ", CutPoint()) +
                       Quote(m_clip.path) +
                       wxString::Format(" -frames:v 1 -vf scale=%d:%d:force_original_aspect_ratio=decrease ",
                                        m_previewSize.x, m_previewSize.y) +
                       Quote(m_previewFile);
        wxArrayString out;
        if (RunSync(cmd, out) != 0 || !wxFileExists(m_previewFile)) return;
        wxImage img;
        if (!img.LoadFile(m_previewFile, wxBITMAP_TYPE_PNG)) return;
        wxBitmap bmp = BlankPreview();
        {
            wxMemoryDC dc(bmp);
            dc.DrawBitmap(wxBitmap(img), (m_previewSize.x - img.GetWidth()) / 2,
                          (m_previewSize.y - img.GetHeight()) / 2);
            wxString label = FormatTime(FrameTime());
            dc.SetFont(MonoFont(8, true));
            wxSize ts = dc.GetTextExtent(label);
            int pad = FromDIP(3);
            dc.SetBrush(wxBrush(wxColour(0, 0, 0, 170)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, m_previewSize.y - ts.y - 2 * pad, m_previewSize.x, ts.y + 2 * pad);
            dc.SetTextForeground(*wxWHITE);
            dc.DrawText(label, (m_previewSize.x - ts.x) / 2, m_previewSize.y - ts.y - pad);
        }
        m_preview->SetBitmap(bmp);
    }

    // <stem>_part1.mp4 + <stem>_part2.mp4, or _part1_2 / _part2_2 ... if taken.
    void PickOutputs() {
        wxString stem = wxFileName(m_clip.path).GetName();
        for (int n = 1; n < 10000; ++n) {
            wxString suffix = n == 1 ? wxString() : wxString::Format("_%d", n);
            wxFileName a(m_saveDir, stem + "_part1" + suffix + ".mp4");
            wxFileName b(m_saveDir, stem + "_part2" + suffix + ".mp4");
            if (!a.FileExists() && !b.FileExists()) {
                m_out1 = a.GetFullPath();
                m_out2 = b.GetFullPath();
                return;
            }
        }
    }

    void StartSplit() {
        if (m_running || !Validate()) return;
        if (!wxDirExists(m_saveDir)) {
            SetStatusText("The Save to folder doesn't exist. Pick one in the main window.", pal::Err);
            return;
        }
        PickOutputs();
        double t = CutPoint();
        wxString fc = wxString::Format(
            "[0:v]split=2[va][vb];"
            "[va]trim=end=%.6f,setpts=PTS-STARTPTS[v1];"
            "[vb]trim=start=%.6f,setpts=PTS-STARTPTS[v2]",
            t, t);
        wxString map1 = " -map [v1]", map2 = " -map [v2]";
        wxString aenc;
        if (m_clip.hasAudio) {
            fc += wxString::Format(
                ";[0:a]asplit=2[aa][ab];"
                "[aa]atrim=end=%.6f,asetpts=PTS-STARTPTS[a1];"
                "[ab]atrim=start=%.6f,asetpts=PTS-STARTPTS[a2]",
                t, t);
            map1 += " -map [a1]";
            map2 += " -map [a2]";
            aenc = " -c:a aac -b:a 192k";
        }
        const wxString venc = " -c:v libx264 -preset veryfast -crf 17 -pix_fmt yuv420p -movflags +faststart ";
        wxString cmd = ToolCmd("ffmpeg") + " -hide_banner -v error -nostats -y -i " +
                       Quote(m_clip.path) + " -filter_complex " + Quote(fc) +
                       map1 + venc + aenc + " " + Quote(m_out1) +
                       map2 + venc + aenc + " " + Quote(m_out2);

        m_stderr.clear();
        m_process = new wxProcess(this);
        m_process->Redirect();
        m_pid = wxExecute(cmd, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE, m_process);
        if (m_pid <= 0) {
            delete m_process;
            m_process = nullptr;
            SetStatusText("Couldn't start ffmpeg.", pal::Err);
            return;
        }
        m_running = true;
        m_reveal->Hide();
        m_gauge->Show();
        m_pulse.Start(80);
        m_split->Disable();
        m_time->Disable();
        SetStatusText("Splitting (frame-exact)...", pal::Text);
        Layout();
    }

    void DrainErr() {
        if (!m_process) return;
        if (wxInputStream* er = m_process->GetErrorStream())
            while (m_process->IsErrorAvailable()) {
                int c = er->GetC();
                if (er->LastRead() == 0) break;
                m_stderr += (char)c;
            }
        if (wxInputStream* in = m_process->GetInputStream())
            while (m_process->IsInputAvailable()) {
                in->GetC();
                if (in->LastRead() == 0) break;
            }
    }

    void RemoveOutputs() {
        if (wxFileExists(m_out1)) wxRemoveFile(m_out1);
        if (wxFileExists(m_out2)) wxRemoveFile(m_out2);
    }

    void OnProcessEnd(wxProcessEvent& e) {
        DrainErr();
        m_pulse.Stop();
        delete m_process;
        m_process = nullptr;
        m_pid = 0;
        m_running = false;
        m_gauge->Hide();
        m_time->Enable();

        bool ok = e.GetExitCode() == 0 && wxFileExists(m_out1) && wxFileExists(m_out2);
        if (!ok) {
            RemoveOutputs();
            wxString err = wxString::FromUTF8(m_stderr.c_str()).Strip(wxString::both).AfterLast('\n');
            SetStatusText("ffmpeg failed: " +
                              (err.empty() ? wxString::Format("exit code %d", e.GetExitCode())
                                           : wxString(err)),
                          pal::Err);
        } else {
            SetStatusText("Done: " + wxFileName(m_out1).GetFullName() + " + _part2", pal::Ok);
            m_reveal->Show();
        }
        m_split->Enable(m_valid);
        Layout();
    }

    void OnClose(wxCloseEvent&) {
        if (m_process) {
            m_process->Detach();
            wxProcess::Kill(m_pid, wxSIGKILL, wxKILL_CHILDREN);
            m_process = nullptr;
            m_pulse.Stop();
            RemoveOutputs();
        }
        m_previewTimer.Stop();
        EndModal(wxID_CANCEL);
    }

    ClipInfo m_clip;
    wxString m_saveDir;
    double m_fps = 30;
    long m_frames = 0;
    long m_cutFrame = 1;
    bool m_valid = false;
    bool m_running = false;

    wxSize m_previewSize;
    wxStaticBitmap* m_preview;
    wxTextCtrl* m_time;
    wxStaticText *m_parts, *m_status;
    wxGauge* m_gauge;
    wxButton *m_split, *m_reveal, *m_close;
    wxTimer m_previewTimer, m_pulse;
    wxString m_previewFile, m_out1, m_out2;

    wxProcess* m_process = nullptr;
    long m_pid = 0;
    std::string m_stderr;
};

// ------------------------------------------------------------------ frame --

class MainFrame : public wxFrame {
public:
    MainFrame() : wxFrame(nullptr, wxID_ANY, "ClipJoiner") {
#ifdef __WXMSW__
        SetIcon(wxICON(JOINY_ICON));
#endif
        SetBackgroundColour(pal::Bg);
        auto* root = new wxPanel(this);
        m_root = root;
        root->SetBackgroundColour(pal::Bg);

        m_slots[0] = new ClipSlot(root, 0);
        m_slots[1] = new ClipSlot(root, 1);

        m_splitBtn = MakeButton(root, "Split...");
        m_splitBtn->SetBitmap(ScissorsIcon());
        m_splitBtn->SetToolTip("Split clip 1 into two parts");
        m_clear = MakeButton(root, "Clear");
        m_join = MakeButton(root, "Join clips");
        m_join->SetFont(MonoFont(10, true));
        m_cancel = MakeButton(root, "Cancel");
        m_cancel->Hide();
        m_reveal = MakeButton(root, "Show in folder");
        m_reveal->Hide();

        // Save-to folder, remembered between runs.
        auto* saveLabel = new wxStaticText(root, wxID_ANY, "Save to:");
        saveLabel->SetFont(MonoFont(9, true));
        saveLabel->SetForegroundColour(pal::Accent);
        m_saveDir = new wxTextCtrl(root, wxID_ANY, LoadSaveDir(), wxDefaultPosition, wxDefaultSize,
                                   wxTE_READONLY | wxBORDER_SIMPLE);
        m_saveDir->SetFont(MonoFont(10));
        m_saveDir->SetBackgroundColour(pal::Slot);
        m_saveDir->SetForegroundColour(pal::Text);
        m_browse = MakeButton(root, "Browse...");

        m_fade = new wxCheckBox(root, wxID_ANY, "0.5s crossfade");
        m_fade->SetFont(MonoFont(10));
        m_fade->SetForegroundColour(pal::Text);
        m_fade->SetValue(wxConfigBase::Get()->ReadBool("Crossfade", false));

        m_gauge = new wxGauge(root, wxID_ANY, 1000, wxDefaultPosition, FromDIP(wxSize(-1, 6)),
                              wxGA_HORIZONTAL | wxGA_SMOOTH);
        m_gauge->Hide();

        m_status = new wxStaticText(root, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                    wxST_ELLIPSIZE_MIDDLE);
        m_status->SetFont(MonoFont(9));
        m_status->SetForegroundColour(pal::Dim);

        auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
        btnRow->Add(m_splitBtn, 0, wxRIGHT, FromDIP(8));
        btnRow->Add(m_clear, 0);
        btnRow->AddStretchSpacer();
        btnRow->Add(m_cancel, 0, wxRIGHT, FromDIP(8));
        btnRow->Add(m_fade, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        btnRow->Add(m_join, 0);

        auto* saveRow = new wxBoxSizer(wxHORIZONTAL);
        saveRow->Add(m_saveDir, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        saveRow->Add(m_browse, 0, wxALIGN_CENTER_VERTICAL);

        auto* s = new wxBoxSizer(wxVERTICAL);
        int pad = FromDIP(16);
        s->Add(m_slots[0], 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
        s->AddSpacer(FromDIP(10));
        s->Add(m_slots[1], 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
        s->Add(saveLabel, 0, wxLEFT | wxRIGHT | wxTOP, pad);
        s->AddSpacer(FromDIP(6));
        s->Add(saveRow, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
        s->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);
        s->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        auto* statusRow = new wxBoxSizer(wxHORIZONTAL);
        statusRow->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        statusRow->Add(m_reveal, 0, wxALIGN_CENTER_VERTICAL);
        s->Add(statusRow, 0, wxEXPAND | wxALL, pad);
        root->SetSizer(s);

        auto* fs = new wxBoxSizer(wxVERTICAL);
        fs->Add(root, 1, wxEXPAND);
        SetSizerAndFit(fs);
        SetMinSize(GetSize());
        Centre();

        root->Bind(EVT_SLOT_FILES, &MainFrame::OnSlotFiles, this);
        m_splitBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OpenSplit(); });
        m_clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_slots[0]->Clear();
            m_slots[1]->Clear();
            SetStatus("", pal::Dim);
            m_reveal->Hide();
            UpdateButtons();
        });
        m_join->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartJoin(); });
        m_browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { BrowseSaveDir(); });
        m_fade->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            wxConfigBase::Get()->Write("Crossfade", m_fade->GetValue());
            wxConfigBase::Get()->Flush();
            UpdateButtons();
            ShowReadyStatus();
        });
        m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CancelJob(); });
        m_reveal->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RevealOutput(); });
        m_timer.Bind(wxEVT_TIMER, &MainFrame::OnTimer, this);
        Bind(wxEVT_END_PROCESS, &MainFrame::OnProcessEnd, this);
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

        UpdateButtons();
        CheckFfmpeg();
    }

    void LoadFiles(const wxArrayString& files) {
        wxCommandEvent ev(EVT_SLOT_FILES);
        ev.SetInt(0);
        ev.SetString(wxJoin(files, '\n', '\0'));
        OnSlotFiles(ev);
    }

private:
    // ---- UI helpers
    wxButton* MakeButton(wxWindow* parent, const wxString& label) {
        auto* b = new wxButton(parent, wxID_ANY, label);
        b->SetFont(MonoFont(10));
        return b;
    }

    static wxString DefaultSaveDir() {
        return wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Desktop);
    }

    static wxString LoadSaveDir() {
        wxString dir = wxConfigBase::Get()->Read("SaveDir", wxString());
        if (dir.empty() || !wxDirExists(dir)) dir = DefaultSaveDir();
        return dir;
    }

    void BrowseSaveDir() {
        wxDirDialog dlg(this, "Choose where joined videos are saved", m_saveDir->GetValue(),
                        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return;
        m_saveDir->SetValue(dlg.GetPath());
        wxConfigBase::Get()->Write("SaveDir", dlg.GetPath());
        wxConfigBase::Get()->Flush();
    }

    // <dir>\<clip1>_joined.mp4, or _joined_2, _joined_3... if taken.
    static wxString UniqueOutput(const wxString& dir, const wxString& stem) {
        wxFileName fn(dir, stem + "_joined.mp4");
        for (int n = 2; fn.FileExists() && n < 10000; ++n)
            fn.SetName(wxString::Format("%s_joined_%d", stem, n));
        return fn.GetFullPath();
    }

    bool FadeOn() const { return m_fade->GetValue(); }

    static bool FadeFits(const ClipInfo& a, const ClipInfo& b) {
        return a.vdur > kFadeSeconds * 2 && b.vdur > kFadeSeconds * 2;
    }

    void SetStatus(const wxString& text, const wxColour& c) {
        m_status->SetForegroundColour(c);
        m_status->SetLabel(text);
        m_status->Refresh();
        Layout();
    }

    void UpdateButtons() {
        bool busy = m_process != nullptr;
        bool both = m_slots[0]->HasClip() && m_slots[1]->HasClip();
        bool any = m_slots[0]->HasClip() || m_slots[1]->HasClip();
        m_join->Enable(!busy && both &&
                       !(FadeOn() && !FadeFits(m_slots[0]->Info(), m_slots[1]->Info())));
        m_splitBtn->Enable(!busy && m_slots[0]->HasClip());
        m_clear->Enable(!busy && any);
        m_browse->Enable(!busy);
        m_fade->Enable(!busy);
        m_slots[0]->SetEnabledLook(!busy);
        m_slots[1]->SetEnabledLook(!busy);
        m_cancel->Show(busy);
        m_gauge->Show(busy);
        m_root->Layout();
    }

    // The "Ready..." line. Only called when the clips or options change, so it
    // never overwrites a Done / error / Cancelled message.
    void ShowReadyStatus() {
        if (m_ffmpegMissing) {
            SetStatus("ffmpeg not found. Put ffmpeg.exe and ffprobe.exe next to ClipJoiner.exe "
                      "or add them to PATH.",
                      pal::Err);
            return;
        }
        bool both = m_slots[0]->HasClip() && m_slots[1]->HasClip();
        if (!both) {
            SetStatus("", pal::Dim);
            return;
        }
        {
            wxString why;
            const ClipInfo &a = m_slots[0]->Info(), &b = m_slots[1]->Info();
            double total = a.duration + b.duration;
            if (FadeOn() && !FadeFits(a, b))
                SetStatus("Each clip must be longer than 1 second for a crossfade.", pal::Err);
            else if (FadeOn())
                SetStatus(wxString::Format("Ready. Total %s with a 0.5s crossfade (re-encode).",
                                           FormatTime(a.vdur + b.vdur - kFadeSeconds)),
                          pal::Ok);
            else if (StreamCopyCompatible(a, b, why))
                SetStatus(wxString::Format("Ready. Total %s. Formats match, fast lossless join.",
                                           FormatTime(total)),
                          pal::Ok);
            else
                SetStatus(wxString::Format("Ready. Total %s. %s, will re-encode to match clip 1.",
                                           FormatTime(total), why),
                          pal::Warn);
        }
    }

    void CheckFfmpeg() {
        wxArrayString out;
        m_ffmpegMissing = RunSync(ToolCmd("ffmpeg") + " -hide_banner -version", out) == -1;
        if (m_ffmpegMissing) ShowReadyStatus();
    }

    // ---- slot handling
    void OnSlotFiles(wxCommandEvent& e) {
        if (m_process) return;
        wxArrayString files = wxSplit(e.GetString(), '\n', '\0');
        int idx = e.GetInt();
        // One file -> that slot. Two or more -> that slot, then the other one.
        for (size_t i = 0; i < files.size() && i < 2; ++i) {
            int target = (idx + (int)i) % 2;
            wxBusyCursor busy;
            m_slots[target]->SetClip(ProbeClip(files[i]));
        }
        m_reveal->Hide();
        UpdateButtons();
        ShowReadyStatus();
        if (files.size() > 2)
            SetStatus("Only two clips are supported for now. Extra files were ignored.", pal::Warn);
    }

    void OpenSplit() {
        if (m_process || !m_slots[0]->HasClip()) return;
        if (!wxDirExists(m_saveDir->GetValue())) {
            SetStatus("The Save to folder doesn't exist. Click Browse... to pick one.", pal::Err);
            return;
        }
        SplitDialog dlg(this, m_slots[0]->Info(), m_saveDir->GetValue());
        dlg.ShowModal();
    }

    static wxBitmapBundle ScissorsIcon() {
        static const char* svg =
            "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 24 24' "
            "fill='none' stroke='#24292f' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
            "<circle cx='7' cy='17' r='3'/><circle cx='17' cy='17' r='3'/>"
            "<path d='M9.15 14.85L18 4'/><path d='M6 4l8.85 10.85'/></svg>";
        return wxBitmapBundle::FromSVG(svg, wxSize(16, 16));
    }

    // ---- join
    void StartJoin() {
        const ClipInfo &a = m_slots[0]->Info(), &b = m_slots[1]->Info();
        if (!a.ok || !b.ok) return;

        wxString dir = m_saveDir->GetValue();
        if (!wxDirExists(dir)) {
            SetStatus("The Save to folder doesn't exist. Click Browse... to pick one.", pal::Err);
            return;
        }
        m_output = UniqueOutput(dir, wxFileName(a.path).GetName());
        // UniqueOutput never returns an existing file, so the inputs are safe,
        // but keep the guard in case of odd paths.
        if (SamePath(m_output, a.path) || SamePath(m_output, b.path)) {
            SetStatus("Output can't overwrite one of the input clips.", pal::Err);
            return;
        }

        wxString why;
        if (FadeOn()) {
            if (!FadeFits(a, b)) return;
            m_totalDur = a.vdur + b.vdur - kFadeSeconds;
            StartEncodeJoin(true);
        } else if (StreamCopyCompatible(a, b, why)) {
            m_totalDur = a.duration + b.duration;
            StartCopyJoin();
        } else {
            m_totalDur = a.duration + b.duration;
            StartEncodeJoin(false);
        }
    }

    static bool SamePath(const wxString& x, const wxString& y) {
        wxFileName fx(x), fy(y);
        fx.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS);
        fy.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS);
        return fx.SameAs(fy);
    }

    static wxString ConcatEscape(const wxString& path) {
        wxString p = path;
        p.Replace("\\", "/");
        p.Replace("'", "'\\''");
        return "file '" + p + "'";
    }

    void StartCopyJoin() {
        m_mode = Mode::Copy;
        m_listFile = wxFileName::CreateTempFileName("clipjoiner");
        wxFile f;
        if (m_listFile.empty() || !f.Open(m_listFile, wxFile::write)) {
            StartEncodeJoin(false);
            return;
        }
        wxString body = ConcatEscape(m_slots[0]->Info().path) + "\n" +
                        ConcatEscape(m_slots[1]->Info().path) + "\n";
        f.Write(body, wxConvUTF8);
        f.Close();

        wxString cmd = ToolCmd("ffmpeg") +
                       " -hide_banner -v error -nostats -progress pipe:1 -y"
                       " -f concat -safe 0 -i " +
                       Quote(m_listFile) + " -c copy -movflags +faststart " + Quote(m_output);
        Launch(cmd, "Joining (lossless copy)...");
    }

    void StartEncodeJoin(bool fade) {
        m_mode = fade ? Mode::Fade : Mode::Encode;
        const ClipInfo &a = m_slots[0]->Info(), &b = m_slots[1]->Info();

        long W = a.w, H = a.h;
        W -= W % 2;  // libx264 + yuv420p need even dimensions
        H -= H % 2;
        wxString fps = a.fpsRaw.empty() ? wxString("30") : a.fpsRaw;
        // settb gives both branches the same timebase, which xfade requires.
        wxString vchain = wxString::Format(
            "scale=%ld:%ld:force_original_aspect_ratio=decrease,"
            "pad=%ld:%ld:(ow-iw)/2:(oh-ih)/2,setsar=1,fps=%s,format=yuv420p,settb=AVTB",
            W, H, W, H, fps);
        // Audio is padded/trimmed to exactly the video length of its clip, so
        // the cut (or fade) lands at the same moment in picture and sound.
        auto achain = [](double len) {
            return wxString::Format(
                "aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,"
                "apad,atrim=0:%.6f,asetpts=PTS-STARTPTS",
                len);
        };

        bool anyAudio = a.hasAudio || b.hasAudio;
        wxString inputs = " -i " + Quote(a.path) + " -i " + Quote(b.path);
        // Silence for any clip without audio, so the audio filters stay happy.
        int nextInput = 2;
        wxString a0 = "[0:a]", a1 = "[1:a]";
        if (anyAudio && !a.hasAudio) {
            inputs += wxString::Format(" -f lavfi -t %.3f -i anullsrc=r=48000:cl=stereo", a.vdur);
            a0 = wxString::Format("[%d:a]", nextInput++);
        }
        if (anyAudio && !b.hasAudio) {
            inputs += wxString::Format(" -f lavfi -t %.3f -i anullsrc=r=48000:cl=stereo", b.vdur);
            a1 = wxString::Format("[%d:a]", nextInput++);
        }

        wxString fc = "[0:v]" + vchain + "[v0];[1:v]" + vchain + "[v1];";
        if (anyAudio)
            fc += a0 + achain(a.vdur) + "[a0];" + a1 + achain(b.vdur) + "[a1];";

        if (fade) {
            double offset = a.vdur - kFadeSeconds;
            fc += wxString::Format("[v0][v1]xfade=transition=fade:duration=%.3f:offset=%.6f[v]",
                                   kFadeSeconds, offset);
            if (anyAudio)
                fc += wxString::Format(";[a0][a1]acrossfade=d=%.3f:c1=tri:c2=tri[a]", kFadeSeconds);
        } else if (anyAudio) {
            fc += "[v0][a0][v1][a1]concat=n=2:v=1:a=1[v][a]";
        } else {
            fc += "[v0][v1]concat=n=2:v=1:a=0[v]";
        }
        wxString maps = anyAudio ? wxString(" -map [v] -map [a] -c:a aac -b:a 192k")
                                 : wxString(" -map [v]");

        wxString cmd = ToolCmd("ffmpeg") + " -hide_banner -v error -nostats -progress pipe:1 -y" +
                       inputs + " -filter_complex " + Quote(fc) + maps +
                       " -c:v libx264 -preset veryfast -crf 17 -pix_fmt yuv420p"
                       " -movflags +faststart " +
                       Quote(m_output);
        Launch(cmd, fade ? wxString("Joining with a 0.5s crossfade...")
                         : wxString("Joining (re-encoding to match clip 1)..."));
    }

    void Launch(const wxString& cmd, const wxString& label) {
        m_stdoutBuf.clear();
        m_stderrBuf.clear();
        m_cancelled = false;
        m_reveal->Hide();
        m_process = new wxProcess(this);
        m_process->Redirect();
        m_pid = wxExecute(cmd, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE, m_process);
        if (m_pid <= 0) {
            delete m_process;
            m_process = nullptr;
            SetStatus("Couldn't start ffmpeg. Is it next to ClipJoiner.exe or on PATH?", pal::Err);
            CleanupListFile();
            UpdateButtons();
            return;
        }
        m_gauge->SetValue(0);
        SetStatus(label, pal::Text);
        UpdateButtons();
        m_timer.Start(100);
    }

    // Drain both pipes without blocking: only read bytes that are available.
    void Drain() {
        if (!m_process) return;
        if (wxInputStream* in = m_process->GetInputStream())
            while (m_process->IsInputAvailable()) {
                int c = in->GetC();
                if (in->LastRead() == 0) break;
                m_stdoutBuf += (char)c;
            }
        if (wxInputStream* er = m_process->GetErrorStream())
            while (m_process->IsErrorAvailable()) {
                int c = er->GetC();
                if (er->LastRead() == 0) break;
                m_stderrBuf += (char)c;
            }
        // ffmpeg -progress writes key=value lines; use the latest out_time_us.
        size_t pos = m_stdoutBuf.rfind("out_time_us=");
        if (pos != std::string::npos && m_totalDur > 0) {
            size_t end = m_stdoutBuf.find('\n', pos);
            if (end != std::string::npos) {
                long long us = atoll(m_stdoutBuf.c_str() + pos + 12);
                double frac = us / 1e6 / m_totalDur;
                if (frac < 0) frac = 0;
                if (frac > 1) frac = 1;
                m_gauge->SetValue((int)(frac * 1000));
                m_stdoutBuf.erase(0, end + 1);
            }
        }
        if (m_stdoutBuf.size() > 65536) m_stdoutBuf.erase(0, m_stdoutBuf.size() - 4096);
    }

    void OnTimer(wxTimerEvent&) { Drain(); }

    void OnProcessEnd(wxProcessEvent& e) {
        m_timer.Stop();
        Drain();
        int code = e.GetExitCode();
        delete m_process;
        m_process = nullptr;
        m_pid = 0;
        CleanupListFile();

        if (m_cancelled) {
            if (wxFileExists(m_output)) wxRemoveFile(m_output);
            SetStatus("Cancelled.", pal::Dim);
            UpdateButtons();
            return;
        }
        if (code != 0 || !wxFileExists(m_output)) {
            if (m_mode == Mode::Copy) {
                // Same-looking formats can still refuse to stream-copy; fall back.
                if (wxFileExists(m_output)) wxRemoveFile(m_output);
                StartEncodeJoin(false);
                return;
            }
            wxString err = wxString::FromUTF8(m_stderrBuf.c_str()).Strip(wxString::both);
            wxString last = err.AfterLast('\n');
            SetStatus("ffmpeg failed: " +
                          (last.empty() ? wxString::Format("exit code %d", code) : wxString(last)),
                      pal::Err);
            UpdateButtons();
            return;
        }
        m_gauge->SetValue(1000);
        wxULongLong bytes = wxFileName::GetSize(m_output);
        double mb = bytes == wxInvalidSize ? 0 : bytes.ToDouble() / (1024.0 * 1024.0);
        wxString how = m_mode == Mode::Copy   ? wxString(" (lossless)")
                       : m_mode == Mode::Fade ? wxString(" (crossfade)")
                                              : wxString();
        SetStatus(wxString::Format("Done%s: %s (%.1f MB)", how,
                                   wxFileName(m_output).GetFullName(), mb),
                  pal::Ok);
        m_reveal->Show();
        UpdateButtons();
    }

    void CancelJob() {
        if (!m_process || m_pid <= 0) return;
        m_cancelled = true;
        wxProcess::Kill(m_pid, wxSIGKILL, wxKILL_CHILDREN);
    }

    void CleanupListFile() {
        if (!m_listFile.empty() && wxFileExists(m_listFile)) wxRemoveFile(m_listFile);
        m_listFile.clear();
    }

    void RevealOutput() {
        if (!wxFileExists(m_output)) return;
#ifdef __WXMSW__
        wxExecute("explorer.exe /select,\"" + m_output + "\"", wxEXEC_ASYNC);
#else
        wxLaunchDefaultApplication(wxFileName(m_output).GetPath());
#endif
    }

    void OnClose(wxCloseEvent& e) {
        if (m_process) {
            if (e.CanVeto() &&
                wxMessageBox("A join is still running. Cancel it and quit?", "ClipJoiner",
                             wxYES_NO | wxICON_QUESTION, this) != wxYES) {
                e.Veto();
                return;
            }
            m_process->Detach();  // the process object deletes itself when ffmpeg exits
            wxProcess::Kill(m_pid, wxSIGKILL, wxKILL_CHILDREN);
            m_process = nullptr;
            if (wxFileExists(m_output)) wxRemoveFile(m_output);
            CleanupListFile();
        }
        m_timer.Stop();
        e.Skip();
    }

    enum class Mode { Copy, Encode, Fade };

    wxPanel* m_root = nullptr;
    ClipSlot* m_slots[2]{};
    wxButton *m_splitBtn, *m_clear, *m_join, *m_cancel, *m_reveal;
    wxGauge* m_gauge;
    wxTextCtrl* m_saveDir;
    wxButton* m_browse;
    wxCheckBox* m_fade;
    wxStaticText* m_status;
    wxTimer m_timer;

    wxProcess* m_process = nullptr;
    long m_pid = 0;
    bool m_cancelled = false;
    bool m_ffmpegMissing = false;
    Mode m_mode = Mode::Copy;
    wxString m_output, m_listFile;
    double m_totalDur = 0;
    std::string m_stdoutBuf, m_stderrBuf;
};

// -------------------------------------------------------------------- app --

class ClipJoinerApp : public wxApp {
public:
    bool OnInit() override {
        SetAppName("ClipJoiner");
        wxInitAllImageHandlers();  // PNG for the split preview  // settings live in HKCU\Software\ClipJoiner on Windows
        auto* frame = new MainFrame();
        frame->Show();
        // Files passed on the command line (or dragged onto ClipJoiner.exe)
        // fill the slots in order.
        wxArrayString files;
        for (int i = 1; i < argc && files.size() < 2; ++i)
            if (wxFileExists(argv[i])) files.Add(argv[i]);
        if (!files.empty()) frame->LoadFiles(files);
        return true;
    }
};

wxIMPLEMENT_APP(ClipJoinerApp);
