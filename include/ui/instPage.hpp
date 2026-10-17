#pragma once
#include <cstdint>
#include <chrono>
#include <unordered_map>
#include <pu/Plutonium>
#include "ui/bottomHint.hpp"
#include "util/install_queue.hpp"

using namespace pu::ui::elm;
namespace inst::ui {
    struct QueueEntry {
        std::string name;
        std::string key;
        std::string detail;
        std::string iconPath;
        u64 iconTitleId = 0;
        std::string iconUrl;
    };
    class instPage : public pu::ui::Layout
    {
        public:
            instPage();
            PU_SMART_CTOR(instPage)
            void onInput(u64 Down, u64 Up, u64 Held, pu::ui::Touch Pos);
            TextBlock::Ref pageInfoText;
            TextBlock::Ref installInfoText;
            pu::ui::elm::ProgressBar::Ref installBar;
            Image::Ref awooImage;
            Image::Ref installIconImage;
            Image::Ref titleImage;
            TextBlock::Ref appVersionText;
            TextBlock::Ref hintText;
            TextBlock::Ref progressText;
            TextBlock::Ref progressDetailText;
            TextBlock::Ref timeText;
            TextBlock::Ref ipText;
            TextBlock::Ref sysLabelText;
            TextBlock::Ref sysFreeText;
            TextBlock::Ref sdLabelText;
            TextBlock::Ref sdFreeText;
            Rectangle::Ref sysBarBack;
            Rectangle::Ref sysBarFill;
            Rectangle::Ref sdBarBack;
            Rectangle::Ref sdBarFill;
            Rectangle::Ref netIndicator;
            Rectangle::Ref wifiBar1;
            Rectangle::Ref wifiBar2;
            Rectangle::Ref wifiBar3;
            Rectangle::Ref batteryOutline;
            Rectangle::Ref batteryFill;
            Rectangle::Ref batteryCap;
            static void setTopInstInfoText(std::string ourText);
            static void setInstInfoText(std::string ourText);
            static void setInstBarPerc(double ourPercent);
            static void setProgressDetailText(const std::string& ourText);
            static void clearProgressDetailText();
            static void setInstallIconFromTitleId(u64 titleId);
            static void setInstallIcon(const std::string& imagePath);
            static void setInstallIconData(const void* imageData, std::uint32_t imageSize);
            static void clearInstallIcon();
            static void loadMainMenu();
            static void loadInstallScreen(bool render = true);
            // Ends what loadInstallScreen started (screen-dim thread, auto-sleep, backlight)
            // for callers that borrow the install screen without a real install, such as
            // the offline DB update. Safe to call when nothing is active.
            static void endInstallScreenSession();
            static void requestInstallCancel();
            static bool isInstallCancelRequested();
            static void clearInstallCancel();
            static void configureQueue(const std::vector<std::string>& names, const std::vector<size_t>& selected);
            static void configureQueue(const std::vector<QueueEntry>& entries, const std::vector<size_t>& selected, std::function<void()> browser = {});
            static bool enqueueTitle(const std::string& key);
            static size_t enqueueTitles(const std::vector<std::string>& keys);
            static bool canEnqueueTitle(const std::string& key);
            static void showQueue();
            bool HasQueuePresentation() const { return !queueEntries.empty(); }
            static std::optional<size_t> nextQueuedItem();
            static void finishQueuedItem();
            static void finishInstall();
        private:
            inst::InstallQueue queue;
            std::vector<std::string> queueNames;
            std::vector<QueueEntry> queueEntries;
            std::unordered_map<std::string, size_t> queueEntryIds;
            std::function<void()> queueBrowser;
            bool queueBrowserOpen = false;
            std::vector<pu::ui::elm::Element::Ref> queueElements;
            TextBlock::Ref activeQueueTitle;
            TextBlock::Ref activeQueueLabel;
            TextBlock::Ref queueProgressLabel;
            TextBlock::Ref queueHeader;
            TextBlock::Ref queueSummary;
            TextBlock::Ref queuePageText;
            TextBlock::Ref queueEmptyText;
            std::vector<Rectangle::Ref> queueRows;
            std::vector<TextBlock::Ref> queueRowNumbers;
            std::vector<TextBlock::Ref> queueRowTitles;
            std::vector<TextBlock::Ref> queueRowDetails;
            std::vector<TextBlock::Ref> queueRowActions;
            std::vector<Image::Ref> queueRowIcons;
            std::vector<std::string> queueRowIconPaths;
            std::vector<size_t> queueRowEntryIds;
            std::vector<bool> queueRowIconLoaded;
            u64 queueIconPollAt = 0;
            void loadQueueIcon(size_t row, size_t entryId);
            void refreshQueueIcons();
            Rectangle::Ref screenBlank;
            bool installActive = false;
            bool suppressWakeTouch = false;
            size_t queueSelection = 0;
            u64 queueHeldDirection = 0;
            u64 queueRepeatAt = 0;
            std::string activeQueueName;
            void refreshQueue();
            bool touchWakeActive = false;
            Rectangle::Ref infoRect;
            Rectangle::Ref topRect;
            Rectangle::Ref botRect;
            BottomHintTouchState bottomHintTouch;
            std::vector<BottomHintSegment> bottomHintSegments;
    };
}
