#include <QWidget>
#include "taskbarProgress.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shobjidl.h>
#endif

namespace {
// SetProgressValue() takes integers — permille is fine-grained enough for any button width.
constexpr quint64 kProgressScale = 1000;
}

TaskbarProgress::TaskbarProgress(QWidget *window)
    : QObject(window)
    , m_window{window}
{}

TaskbarProgress::~TaskbarProgress()
{
#ifdef Q_OS_WIN
    if (m_taskbar)
        static_cast<ITaskbarList3 *>(m_taskbar)->Release();
#endif
}

void TaskbarProgress::setProgress(State state, double fraction)
{
    // Called about once per second with mostly the same values — skip the COM round trip.
    const double clamped = qBound(0.0, fraction, 1.0);
    if (state == m_state && qAbs(clamped - m_fraction) < 1.0 / kProgressScale)
        return;

    m_state    = state;
    m_fraction = clamped;
    apply();
}

void TaskbarProgress::reset()
{
#ifdef Q_OS_WIN
    if (m_taskbar) {
        static_cast<ITaskbarList3 *>(m_taskbar)->Release();
        m_taskbar = nullptr;
    }
#endif
    apply();
}

void TaskbarProgress::apply()
{
#ifdef Q_OS_WIN
    // Nothing to show and never shown: no need to create the interface at all.
    if (!m_taskbar && m_state == State::None)
        return;

    if (!m_taskbar) {
        // COM is already initialized on the GUI thread — Qt calls OleInitialize().
        ITaskbarList3 *taskbar = nullptr;
        if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&taskbar))))
            return;
        if (FAILED(taskbar->HrInit())) {
            taskbar->Release();
            return;
        }
        m_taskbar = taskbar;
    }

    auto *taskbar = static_cast<ITaskbarList3 *>(m_taskbar);
    const HWND hwnd = reinterpret_cast<HWND>(m_window->winId());

    TBPFLAG flag = TBPF_NOPROGRESS;
    switch (m_state) {
    case State::None:   flag = TBPF_NOPROGRESS; break;
    case State::Normal: flag = TBPF_NORMAL;     break;
    case State::Paused: flag = TBPF_PAUSED;     break;
    case State::Error:  flag = TBPF_ERROR;      break;
    }

    taskbar->SetProgressState(hwnd, flag);
    if (m_state != State::None)
        taskbar->SetProgressValue(hwnd, ULONGLONG(m_fraction * kProgressScale), ULONGLONG(kProgressScale));
#endif
}
