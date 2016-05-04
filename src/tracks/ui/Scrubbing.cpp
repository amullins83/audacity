/**********************************************************************

Audacity: A Digital Audio Editor

Scrubbing.cpp

Paul Licameli split from TrackPanel.cpp

**********************************************************************/

#include "Scrubbing.h"
#include "../../Experimental.h"
#include <functional>

#include "../../AudioIO.h"
#include "../../Project.h"
#include "../../TrackPanel.h"
#include "../../TrackPanelCell.h"
#include "../../TrackPanelCellIterator.h"
#include "../../commands/CommandFunctors.h"
#include "../../toolbars/ControlToolBar.h"
#include "../../widgets/Ruler.h"

#include <algorithm>

#include <wx/dc.h>

// Conditional compilation switch for making scrub menu items checkable
#define CHECKABLE_SCRUB_MENU_ITEMS

enum {
   // PRL:
   // Mouse must move at least this far to distinguish ctrl-drag to scrub
   // from ctrl-click for playback.
   SCRUBBING_PIXEL_TOLERANCE = 10,

#ifdef EXPERIMENTAL_SCRUBBING_SCROLL_WHEEL
   ScrubSpeedStepsPerOctave = 4,
#endif
};

namespace {
   double FindScrubbingSpeed(const ViewInfo &viewInfo, double maxScrubSpeed, double screen, double timeAtMouse)
   {
      // Map a time (which was mapped from a mouse position)
      // to a speed.
      // Map times to positive and negative speeds,
      // with the time at the midline of the screen mapping to 0,
      // and the extremes to the maximum scrub speed.

      // Width of visible track area, in time terms:
      const double origin = viewInfo.h + screen / 2.0;

      // There are various snapping zones that are this fraction of screen:
      const double snap = 0.05;

      // By shrinking denom a bit, we make margins left and right
      // that snap to maximum and negative maximum speeds.
      const double factor = 1.0 - (snap * 2);
      const double denom = factor * screen / 2.0;
      double fraction = std::min(1.0, fabs(timeAtMouse - origin) / denom);

      // Snap to 1.0 and -1.0
      const double unity = 1.0 / maxScrubSpeed;
      const double tolerance = snap / factor;
      // Make speeds near 1 available too by remapping fractions outside
      // this snap zone
      if (fraction <= unity - tolerance)
         fraction *= unity / (unity - tolerance);
      else if (fraction < unity + tolerance)
         fraction = unity;
      else
         fraction = unity + (fraction - (unity + tolerance)) *
         (1.0 - unity) / (1.0 - (unity + tolerance));

      double result = fraction * maxScrubSpeed;
      if (timeAtMouse < origin)
         result *= -1.0;
      return result;
   }

   double FindSeekSpeed(const ViewInfo &viewInfo, double maxScrubSpeed, double screen, double timeAtMouse)
   {
      // Map a time (which was mapped from a mouse position)
      // to a signed skip speed: a multiplier of the stutter duration,
      // by which to advance the play position.
      // (The stutter will play at unit speed.)

      // Times near the midline of the screen map to skip-less play,
      // and the extremes to a value proportional to maximum scrub speed.

      // If the maximum scrubbing speed defaults to 1.0 when you begin to scroll-scrub,
      // the extreme skipping for scroll-seek needs to be larger to be useful.
      static const double ARBITRARY_MULTIPLIER = 10.0;
      const double extreme = std::max(1.0, maxScrubSpeed * ARBITRARY_MULTIPLIER);

      // Width of visible track area, in time terms:
      const double halfScreen = screen / 2.0;
      const double origin = viewInfo.h + halfScreen;

      // The snapping zone is this fraction of screen, on each side of the
      // center line:
      const double snap = 0.05;
      const double fraction =
         std::max(snap, std::min(1.0, fabs(timeAtMouse - origin) / halfScreen));

      double result = 1.0 + ((fraction - snap) / (1.0 - snap)) * (extreme - 1.0);
      if (timeAtMouse < origin)
         result *= -1.0;
      return result;
   }
}

Scrubber::Scrubber(AudacityProject *project)
   : mScrubToken(-1)
   , mScrubStartClockTimeMillis(-1)
   , mScrubHasFocus(false)
   , mScrubSpeedDisplayCountdown(0)
   , mScrubStartPosition(-1)
   , mMaxScrubSpeed(-1.0)
   , mScrubSeekPress(false)
#ifdef EXPERIMENTAL_SCRUBBING_SCROLL_WHEEL
   , mSmoothScrollingScrub(false)
   , mLogMaxScrubSpeed(0)
#endif

   , mProject(project)
{
   if (wxTheApp)
      wxTheApp->Connect
      (wxEVT_ACTIVATE_APP,
      wxActivateEventHandler(Scrubber::OnActivateOrDeactivateApp), NULL, this);
   mProject->PushEventHandler(&mForwarder);
}

Scrubber::~Scrubber()
{
   mProject->PopEventHandler();
   if (wxTheApp)
      wxTheApp->Disconnect
      (wxEVT_ACTIVATE_APP,
      wxActivateEventHandler(Scrubber::OnActivateOrDeactivateApp), NULL, this);
}

namespace {
   const struct MenuItem {
      wxString name;
      wxString label;
      wxString status;
      void (Scrubber::*memFn)(wxCommandEvent&);
      bool scroll;
      bool seek;

      const wxString &GetStatus() const { return status; }
   } menuItems[] = {
      /* i18n-hint: These commands assist the user in finding a sound by ear. ...
         "Scrubbing" is variable-speed playback, ...
         "Seeking" is normal speed playback but with skips, ...
         "Scrolling" keeps the playback position at a fixed place on screen while the waveform moves
       */
      { wxT("Scrub"),       XO("&Scrub"),           XO("Scrubbing"),
         &Scrubber::OnScrub,       false,        false },

      { wxT("ScrollScrub"), XO("Sc&rolling Scrub"), XO("Scrolling Scrub"),
         &Scrubber::OnScrollScrub, true,         false },

      { wxT("Seek"),        XO("See&k"),            XO("Seeking"),
         &Scrubber::OnSeek,        false,        true  },

      { wxT("ScrollSeek"),  XO("Scro&lling Seek"),  XO("Scrolling Seek"),
         &Scrubber::OnScrollSeek,  true,         true  }
   };

   enum { nMenuItems = sizeof(menuItems) / sizeof(*menuItems) };

   inline const MenuItem &FindMenuItem(bool scroll, bool seek)
   {
      return *std::find_if(menuItems, menuItems + nMenuItems,
         [=](const MenuItem &item) {
            return scroll == item.scroll &&
               seek == item.seek;
         }
      );
   }

}

void Scrubber::MarkScrubStart(
   // Assume xx is relative to the left edge of TrackPanel!
   wxCoord xx
#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
   , bool smoothScrolling
#endif
   , bool alwaysSeeking
)
{
   UncheckAllMenuItems();

   // Don't actually start scrubbing, but collect some information
   // needed for the decision to start scrubbing later when handling
   // drag events.
#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
   SetScrollScrubbing (smoothScrolling);
#endif
   mAlwaysSeeking = alwaysSeeking;
   mScrubStartPosition = xx;
   mScrubStartClockTimeMillis = ::wxGetLocalTimeMillis();

   ControlToolBar * const ctb = mProject->GetControlToolBar();
   ctb->SetPlay(true, ControlToolBar::PlayAppearance::Scrub);
   ctb->UpdateStatusBar(mProject);

   CheckMenuItem();
}

#ifdef EXPERIMENTAL_SCRUBBING_SUPPORT
// Assume xx is relative to the left edge of TrackPanel!
bool Scrubber::MaybeStartScrubbing(wxCoord xx)
{
   if (mScrubStartPosition < 0)
      return false;
   if (IsScrubbing())
      return false;
   else {
      const bool busy = gAudioIO->IsBusy();
      if (busy && gAudioIO->GetNumCaptureChannels() > 0) {
         // Do not stop recording, and don't try to start scrubbing after
         // recording stops
         mScrubStartPosition = -1;
         return false;
      }

      wxCoord position = xx;
      if (abs(mScrubStartPosition - position) >= SCRUBBING_PIXEL_TOLERANCE) {
         const ViewInfo &viewInfo = mProject->GetViewInfo();
         TrackPanel *const trackPanel = mProject->GetTrackPanel();
         ControlToolBar * const ctb = mProject->GetControlToolBar();
         double maxTime = mProject->GetTracks()->GetEndTime();
         const int leftOffset = trackPanel->GetLeftOffset();
         double time0 = std::min(maxTime,
            viewInfo.PositionToTime(mScrubStartPosition, leftOffset)
         );
         double time1 = std::min(maxTime,
            viewInfo.PositionToTime(position, leftOffset)
         );
         if (time1 != time0)
         {
            if (busy)
               ctb->StopPlaying();

            AudioIOStartStreamOptions options(mProject->GetDefaultPlayOptions());
            options.timeTrack = NULL;
            options.scrubDelay = (kTimerInterval / 1000.0);
            options.scrubStartClockTimeMillis = mScrubStartClockTimeMillis;
            options.minScrubStutter = 0.2;
#if 0
            if (!mAlwaysSeeking) {
               // Take the starting speed limit from the transcription toolbar,
               // but it may be varied during the scrub.
               mMaxScrubSpeed = options.maxScrubSpeed =
               p->GetTranscriptionToolBar()->GetPlaySpeed();
            }
#else
            // That idea seems unpopular... just make it one
            mMaxScrubSpeed = options.maxScrubSpeed = 1.0;
#endif
            options.maxScrubTime = mProject->GetTracks()->GetEndTime();
            ControlToolBar::PlayAppearance appearance =
               ControlToolBar::PlayAppearance::Scrub;
            const bool cutPreview = false;
            const bool backwards = time1 < time0;
#ifdef EXPERIMENTAL_SCRUBBING_SCROLL_WHEEL
            static const double maxScrubSpeedBase =
               pow(2.0, 1.0 / ScrubSpeedStepsPerOctave);
            mLogMaxScrubSpeed = floor(0.5 +
               log(mMaxScrubSpeed) / log(maxScrubSpeedBase)
            );
#endif
            mScrubSpeedDisplayCountdown = 0;
            mScrubToken =
               ctb->PlayPlayRegion(SelectedRegion(time0, time1), options,
                                   PlayMode::normalPlay, appearance, backwards);
         }
      }
      else
         // Wait to test again
         mScrubStartClockTimeMillis = ::wxGetLocalTimeMillis();

      if (IsScrubbing())
         mScrubHasFocus = true;

      // Return true whether we started scrub, or are still waiting to decide.
      return true;
   }
}

void Scrubber::ContinueScrubbing()
{

   // Thus scrubbing relies mostly on periodic polling of mouse and keys,
   // not event notifications.  But there are a few event handlers that
   // leave messages for this routine, in mScrubSeekPress and in mScrubHasFocus.

   // Seek only when the pointer is in the panel.  Else, scrub.
   const wxMouseState state(::wxGetMouseState());
   TrackPanel *const trackPanel = mProject->GetTrackPanel();

   // Decide whether to skip play, because either mouse is down now,
   // or there was a left click event.  (This is then a delayed reaction, in a
   // timer callback, to a left click event detected elsewhere.)
   const bool seek = PollIsSeeking() || mScrubSeekPress;

   {
      // Show the correct status for seeking.
      bool backup = mAlwaysSeeking;
      mAlwaysSeeking = seek;
      const auto ctb = mProject->GetControlToolBar();
      ctb->UpdateStatusBar(mProject);
      mAlwaysSeeking = backup;
   }

   const wxPoint position = trackPanel->ScreenToClient(state.GetPosition());
   // When we don't have focus, enqueue silent scrubs until we regain focus.
   bool result = false;
   if (!mScrubHasFocus)
      result = gAudioIO->EnqueueScrubBySignedSpeed(0, mMaxScrubSpeed, false);
   else {
      const double time = mProject->GetViewInfo().PositionToTime(position.x, trackPanel->GetLeftOffset());

      if (seek)
         // Cause OnTimer() to suppress the speed display
         mScrubSpeedDisplayCountdown = 1;

#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
      if (mSmoothScrollingScrub) {
         const double speed = FindScrubSpeed(seek, time);
         result = gAudioIO->EnqueueScrubBySignedSpeed(speed, mMaxScrubSpeed, seek);
      }
      else
#endif
         result = gAudioIO->EnqueueScrubByPosition
         (time, seek ? 1.0 : mMaxScrubSpeed, seek);
   }

   if (result)
      mScrubSeekPress = false;
   // else, if seek requested, try again at a later time when we might
   // enqueue a long enough stutter

#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
   if (mSmoothScrollingScrub)
      ;
   else
#endif
   {
      if (mScrubSpeedDisplayCountdown > 0)
         --mScrubSpeedDisplayCountdown;
   }
}

void Scrubber::StopScrubbing()
{
   UncheckAllMenuItems();

   mScrubStartPosition = -1;
   SetScrollScrubbing (false);

   if (!IsScrubbing())
   {
      // Marked scrub start, but
      // didn't really play, but did change button apperance
      const auto ctb = mProject->GetControlToolBar();
      ctb->SetPlay(false, ControlToolBar::PlayAppearance::Straight);
   }
}

void Scrubber::SetScrollScrubbing(bool scrollScrubbing)
{
   mSmoothScrollingScrub = scrollScrubbing;
   mProject->GetPlaybackScroller().Activate(scrollScrubbing);
}

bool Scrubber::IsScrubbing() const
{
   if (mScrubToken <= 0)
      return false;
   else if (mScrubToken == mProject->GetAudioIOToken())
      return true;
   else {
      const_cast<Scrubber&>(*this).mScrubToken = -1;
      const_cast<Scrubber&>(*this).mScrubStartPosition = -1;
#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
      // Don't call SetScrollScrubbing
      const_cast<Scrubber&>(*this).mSmoothScrollingScrub = false;
#endif
      return false;
   }
}

bool Scrubber::ShouldDrawScrubSpeed()
{
   return IsScrubbing() &&
      mScrubHasFocus && (
      // Draw for (non-scroll) scrub, sometimes, but never for seek
      (!PollIsSeeking() && mScrubSpeedDisplayCountdown > 0)
#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
      // Draw always for scroll-scrub and for scroll-seek
       || mSmoothScrollingScrub
#endif
      );
}

double Scrubber::FindScrubSpeed(bool seeking, double time) const
{
   ViewInfo &viewInfo = mProject->GetViewInfo();
   const double screen = mProject->GetScreenEndTime() - viewInfo.h;
   return (seeking ? FindSeekSpeed : FindScrubbingSpeed)
      (viewInfo, mMaxScrubSpeed, screen, time);
}

void Scrubber::HandleScrollWheel(int steps)
{
   const int newLogMaxScrubSpeed = mLogMaxScrubSpeed + steps;
   static const double maxScrubSpeedBase =
      pow(2.0, 1.0 / ScrubSpeedStepsPerOctave);
   double newSpeed = pow(maxScrubSpeedBase, newLogMaxScrubSpeed);
   if (newSpeed >= AudioIO::GetMinScrubSpeed() &&
      newSpeed <= AudioIO::GetMaxScrubSpeed()) {
      mLogMaxScrubSpeed = newLogMaxScrubSpeed;
      mMaxScrubSpeed = newSpeed;
#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
      if (!mSmoothScrollingScrub)
#endif
         // Show the speed for one second
         mScrubSpeedDisplayCountdown = kOneSecondCountdown + 1;
   }
}

void Scrubber::OnActivateOrDeactivateApp(wxActivateEvent &event)
{
   if (event.GetActive())
      mScrubHasFocus = IsScrubbing();
   else
      mScrubHasFocus = false;

   event.Skip();
}

void Scrubber::Forwarder::OnMouse(wxMouseEvent &event)
{
   auto isScrubbing = scrubber.IsScrubbing();
   if (!isScrubbing && scrubber.HasStartedScrubbing()) {
      if (!event.HasAnyModifiers() &&
          event.GetEventType() == wxEVT_MOTION) {

         // Really start scrub if motion is far enough
         auto ruler = scrubber.mProject->GetRulerPanel();
         auto xx = ruler->ScreenToClient(::wxGetMousePosition()).x;
         scrubber.MaybeStartScrubbing(xx);
      }
   }
   else if (isScrubbing && !event.HasAnyModifiers()) {
      if(event.LeftDown() ||
         (event.LeftIsDown() && event.Dragging())) {
         scrubber.mScrubSeekPress = true;
         auto ruler = scrubber.mProject->GetRulerPanel();
         auto xx = ruler->ScreenToClient(::wxGetMousePosition()).x;
         ruler->UpdateQuickPlayPos(xx);
      }
      else if (event.m_wheelRotation) {
         double steps = event.m_wheelRotation /
         (event.m_wheelDelta > 0 ? (double)event.m_wheelDelta : 120.0);
         scrubber.HandleScrollWheel(steps);
      }
      else
         event.Skip();
   }
   else
      event.Skip();
}

///////////////////////////////////////////////////////////////////////////////
// class ScrubbingOverlay is responsible for drawing the speed numbers

ScrubbingOverlay::ScrubbingOverlay(AudacityProject *project)
   : mProject(project)
   , mLastScrubRect()
   , mNextScrubRect()
   , mLastScrubSpeedText()
   , mNextScrubSpeedText()
{
   mProject->Connect(EVT_TRACK_PANEL_TIMER,
      wxCommandEventHandler(ScrubbingOverlay::OnTimer),
      NULL,
      this);
}

ScrubbingOverlay::~ScrubbingOverlay()
{
   mProject->Disconnect(EVT_TRACK_PANEL_TIMER,
      wxCommandEventHandler(ScrubbingOverlay::OnTimer),
      NULL,
      this);
}

std::pair<wxRect, bool> ScrubbingOverlay::DoGetRectangle(wxSize)
{
   wxRect rect(mLastScrubRect);
   const bool outdated =
      (mLastScrubRect != mNextScrubRect) ||
      (!mLastScrubRect.IsEmpty() && !GetScrubber().ShouldDrawScrubSpeed()) ||
      (mLastScrubSpeedText != mNextScrubSpeedText);
   return std::make_pair(
      rect,
      outdated
   );
}

void ScrubbingOverlay::Draw
   (wxDC &dc, TrackPanelCellIterator, TrackPanelCellIterator)
{
   mLastScrubRect = mNextScrubRect;
   mLastScrubSpeedText = mNextScrubSpeedText;

   Scrubber &scrubber = GetScrubber();
   if (!scrubber.ShouldDrawScrubSpeed())
      return;

   static const wxFont labelFont(24, wxSWISS, wxNORMAL, wxNORMAL);
   dc.SetFont(labelFont);

   // These two colors were previously saturated red and green.  However
   // we have a rule to try to only use red for reserved purposes of
   //  (a) Recording
   //  (b) Error alerts
   // So they were changed to 'orange' and 'lime'.
   static const wxColour clrNoScroll(215, 162, 0), clrScroll(0, 204, 153);
#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
   if (scrubber.IsScrollScrubbing())
      dc.SetTextForeground(clrScroll);
   else
#endif
      dc.SetTextForeground(clrNoScroll);

   dc.DrawText(mLastScrubSpeedText, mLastScrubRect.GetX(), mLastScrubRect.GetY());
}

void ScrubbingOverlay::OnTimer(wxCommandEvent &event)
{
   // Let other listeners get the notification
   event.Skip();

   Scrubber &scrubber = GetScrubber();
   if (!GetScrubber().IsScrubbing()) {
      mNextScrubRect = wxRect();
      return;
   }

   // Call ContinueScrubbing() here in the timer handler
   // rather than in SelectionHandleDrag()
   // so that even without drag events, we can instruct the play head to
   // keep approaching the mouse cursor, when its maximum speed is limited.
   scrubber.ContinueScrubbing();

   if (!scrubber.ShouldDrawScrubSpeed()) {
      mNextScrubRect = wxRect();
   }
   else {
      TrackPanel *const trackPanel = mProject->GetTrackPanel();
      int panelWidth, panelHeight;
      trackPanel->GetSize(&panelWidth, &panelHeight);

      // Where's the mouse?
      int xx, yy;
      ::wxGetMousePosition(&xx, &yy);
      trackPanel->ScreenToClient(&xx, &yy);

      const bool seeking = scrubber.PollIsSeeking();

      // Find the text
      const double maxScrubSpeed = GetScrubber().GetMaxScrubSpeed();
      const double speed =
#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
         scrubber.IsScrollScrubbing()
         ? scrubber.FindScrubSpeed
            (seeking, mProject->GetViewInfo().PositionToTime(xx, trackPanel->GetLeftOffset()))
         :
#endif
            maxScrubSpeed;

      const wxChar *format =
#ifdef EXPERIMENTAL_SCRUBBING_SMOOTH_SCROLL
         scrubber.IsScrollScrubbing()
         ? seeking
            ? wxT("%+.2fX")
            : wxT("%+.2f")
         :
#endif
            wxT("%.2f");

      mNextScrubSpeedText = wxString::Format(format, speed);

      // Find the origin for drawing text
      wxCoord width, height;
      {
         wxClientDC dc(trackPanel);
         static const wxFont labelFont(24, wxSWISS, wxNORMAL, wxNORMAL);
         dc.SetFont(labelFont);
         dc.GetTextExtent(mNextScrubSpeedText, &width, &height);
      }
      xx = std::max(0, std::min(panelWidth - width, xx - width / 2));

      // Put the text above the cursor, if it fits.
      enum { offset = 20 };
      yy -= height + offset;
      if (yy < 0)
         yy += height + 2 * offset;
      yy = std::max(0, std::min(panelHeight - height, yy));

      mNextScrubRect = wxRect(xx, yy, width, height);
   }
}

const Scrubber &ScrubbingOverlay::GetScrubber() const
{
   return mProject->GetScrubber();
}

Scrubber &ScrubbingOverlay::GetScrubber()
{
   return mProject->GetScrubber();
}

bool Scrubber::PollIsSeeking()
{
   return mAlwaysSeeking || ::wxGetMouseState().LeftIsDown();
}

void Scrubber::DoScrub(bool scroll, bool seek)
{
   const bool wasScrubbing = IsScrubbing();
   const bool match = (scroll == mSmoothScrollingScrub && seek == mAlwaysSeeking);
   if (!wasScrubbing) {
      auto tp = mProject->GetTrackPanel();
      wxCoord xx = tp->ScreenToClient(::wxGetMouseState().GetPosition()).x;

      // Limit x
      int width;
      tp->GetTracksUsableArea(&width, nullptr);
      const auto offset = tp->GetLeftOffset();
      xx = (std::max(offset, std::min(offset + width - 1, xx)));

      MarkScrubStart(xx, scroll, seek);
   }
   else if(!match) {
      SetScrollScrubbing(scroll);
      mAlwaysSeeking = seek;
      UncheckAllMenuItems();
      CheckMenuItem();

      // Show the correct status.
      const auto ctb = mProject->GetControlToolBar();
      ctb->UpdateStatusBar(mProject);
   }
   else {
      // This will call back to Scrubber::StopScrubbing
      const auto ctb = mProject->GetControlToolBar();
      ctb->StopPlaying();
   }
}

void Scrubber::OnScrub(wxCommandEvent&)
{
   DoScrub(false, false);
}

void Scrubber::OnScrollScrub(wxCommandEvent&)
{
   DoScrub(true, false);
}

void Scrubber::OnSeek(wxCommandEvent&)
{
   DoScrub(false, true);
}

void Scrubber::OnScrollSeek(wxCommandEvent&)
{
   DoScrub(true, true);
}

enum { CMD_ID = 8000 };

BEGIN_EVENT_TABLE(Scrubber, wxEvtHandler)
   EVT_MENU(CMD_ID,     Scrubber::OnScrub)
   EVT_MENU(CMD_ID + 1, Scrubber::OnScrollScrub)
   EVT_MENU(CMD_ID + 2, Scrubber::OnSeek)
   EVT_MENU(CMD_ID + 3, Scrubber::OnScrollSeek)
END_EVENT_TABLE()

BEGIN_EVENT_TABLE(Scrubber::Forwarder, wxEvtHandler)
   EVT_MOUSE_EVENTS(Scrubber::Forwarder::OnMouse)
END_EVENT_TABLE()

static_assert(nMenuItems == 4, "wrong number of items");

const wxString &Scrubber::GetUntranslatedStateString() const
{
   static wxString empty;

   if (HasStartedScrubbing()) {
      auto &item = FindMenuItem(mSmoothScrollingScrub, mAlwaysSeeking);
      return item.status;
   }
   else
      return empty;
}

std::vector<wxString> Scrubber::GetAllUntranslatedStatusStrings()
{
   using namespace std;
   vector<wxString> results;
   transform(menuItems, menuItems + nMenuItems, back_inserter(results),
             mem_fun_ref(&MenuItem::GetStatus));
   return move(results);
}

bool Scrubber::CanScrub() const
{
   auto cm = mProject->GetCommandManager();
   return cm->GetEnabled(menuItems[0].name);
}

void Scrubber::AddMenuItems()
{
   auto cm = mProject->GetCommandManager();
   auto flags = cm->GetDefaultFlags() | WaveTracksExistFlag;
   auto mask = cm->GetDefaultMask() | WaveTracksExistFlag;

   cm->BeginSubMenu(_("Scru&bbing"));
   for (const auto &item : menuItems) {
#ifdef CHECKABLE_SCRUB_MENU_ITEMS
      cm->AddCheck(item.name, wxGetTranslation(item.label),
                  FNT(Scrubber, this, item.memFn),
                  false, flags, mask);
#else
      cm->AddItem(item.name, wxGetTranslation(item.label),
                   FNT(Scrubber, this, item.memFn),
                   flags, mask);
#endif
   }
   cm->EndSubMenu();
   CheckMenuItem();
}

void Scrubber::PopulateMenu(wxMenu &menu)
{
   int id = CMD_ID;
   auto cm = mProject->GetCommandManager();
   const MenuItem *checkedItem =
      HasStartedScrubbing()
         ? &FindMenuItem(mSmoothScrollingScrub, mAlwaysSeeking)
         : nullptr;
   for (const auto &item : menuItems) {
      if (cm->GetEnabled(item.name)) {
#ifdef CHECKABLE_SCRUB_MENU_ITEMS
         menu.AppendCheckItem(id, item.label);
         if(&item == checkedItem)
            menu.FindItem(id)->Check();
#else
         menu.Append(id, item.label);
#endif
      }
      ++id;
   }
}

void Scrubber::UncheckAllMenuItems()
{
#ifdef CHECKABLE_SCRUB_MENU_ITEMS
   auto cm = mProject->GetCommandManager();
   for (const auto &item : menuItems)
      cm->Check(item.name, false);
#endif
}

void Scrubber::CheckMenuItem()
{
#ifdef CHECKABLE_SCRUB_MENU_ITEMS
   if(HasStartedScrubbing()) {
      auto cm = mProject->GetCommandManager();
      auto item = FindMenuItem(mSmoothScrollingScrub, mAlwaysSeeking);
      cm->Check(item.name, true);
   }
#endif
}

#endif
