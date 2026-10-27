#pragma once

#include <QObject>

class QWidget;

/**
 * @brief Progress bar on the window's taskbar button (Windows ITaskbarList3).
 *
 * Qt 6 has no QWinTaskbarProgress any more, so the COM interface is used
 * directly. The interface is created on first use — the taskbar button must
 * exist by then — and again after Explorer restarted (reset()).
 * Other platforms: every call is a no-op.
 */
class TaskbarProgress : public QObject
{
    Q_OBJECT

public:
    /// What the taskbar button shows.
    enum class State {
        None,       ///< No progress bar.
        Normal,     ///< Green bar.
        Paused,     ///< Yellow bar.
        Error       ///< Red bar — a job of the batch failed.
    };

    /// @p window: the top-level window whose taskbar button shows the progress.
    explicit TaskbarProgress(QWidget *window);
    ~TaskbarProgress() override;

    /// @brief Show @p state with @p fraction (0..1) done.
    void setProgress(State state, double fraction);

    /// @brief Drop the COM interface and re-apply the last state — call on the
    /// "TaskbarButtonCreated" message (new button after an Explorer restart).
    void reset();

private:
    QWidget *m_window   = nullptr;
    void    *m_taskbar  = nullptr;   ///< ITaskbarList3*, created lazily.
    State    m_state    = State::None;
    double   m_fraction = 0.0;

    /// @brief Push m_state / m_fraction to the taskbar button.
    void apply();
};
