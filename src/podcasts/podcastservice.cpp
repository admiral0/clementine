/* This file is part of Clementine.
   Copyright 2012, David Sansome <me@davidsansome.com>
   
   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   
   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "addpodcastdialog.h"
#include "opmlcontainer.h"
#include "podcastbackend.h"
#include "podcastdownloader.h"
#include "podcastservice.h"
#include "podcastservicemodel.h"
#include "podcastupdater.h"
#include "core/application.h"
#include "core/logging.h"
#include "core/mergedproxymodel.h"
#include "internet/internetmodel.h"
#include "library/libraryview.h"
#include "ui/iconloader.h"
#include "ui/standarditemiconloader.h"

#include <QMenu>
#include <QSortFilterProxyModel>

const char* PodcastService::kServiceName = "Podcasts";
const char* PodcastService::kSettingsGroup = "Podcasts";


class PodcastSortProxyModel : public QSortFilterProxyModel {
public:
  PodcastSortProxyModel(QObject* parent = NULL);

protected:
  bool lessThan(const QModelIndex& left, const QModelIndex& right) const;
};


PodcastService::PodcastService(Application* app, InternetModel* parent)
  : InternetService(kServiceName, app, parent, parent),
    use_pretty_covers_(true),
    icon_loader_(new StandardItemIconLoader(app->album_cover_loader(), this)),
    backend_(app->podcast_backend()),
    model_(new PodcastServiceModel(this)),
    proxy_(new PodcastSortProxyModel(this)),
    context_menu_(NULL),
    root_(NULL)
{
  icon_loader_->SetModel(model_);
  proxy_->setSourceModel(model_);
  proxy_->setDynamicSortFilter(true);
  proxy_->sort(0);

  connect(backend_, SIGNAL(SubscriptionAdded(Podcast)), SLOT(SubscriptionAdded(Podcast)));
  connect(backend_, SIGNAL(SubscriptionRemoved(Podcast)), SLOT(SubscriptionRemoved(Podcast)));
  connect(backend_, SIGNAL(EpisodesAdded(QList<PodcastEpisode>)), SLOT(EpisodesAdded(QList<PodcastEpisode>)));
  connect(backend_, SIGNAL(EpisodesUpdated(QList<PodcastEpisode>)), SLOT(EpisodesUpdated(QList<PodcastEpisode>)));

  connect(app_->playlist_manager(), SIGNAL(CurrentSongChanged(Song)), SLOT(CurrentSongChanged(Song)));
}

PodcastService::~PodcastService() {
}

PodcastSortProxyModel::PodcastSortProxyModel(QObject* parent)
  : QSortFilterProxyModel(parent) {
}

bool PodcastSortProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
  const int left_type  = left.data(InternetModel::Role_Type).toInt();
  const int right_type = right.data(InternetModel::Role_Type).toInt();

  // The special Add Podcast item comes first
  if (left_type == PodcastService::Type_AddPodcast)
    return true;
  else if (right_type == PodcastService::Type_AddPodcast)
    return false;

  // Otherwise we only compare identical typed items.
  if (left_type != right_type)
    return QSortFilterProxyModel::lessThan(left, right);

  switch (left_type) {
  case PodcastService::Type_Podcast:
    return left.data().toString().localeAwareCompare(right.data().toString()) < 0;

  case PodcastService::Type_Episode: {
    const PodcastEpisode left_episode  = left.data(PodcastService::Role_Episode).value<PodcastEpisode>();
    const PodcastEpisode right_episode = right.data(PodcastService::Role_Episode).value<PodcastEpisode>();

    return left_episode.publication_date() > right_episode.publication_date();
  }

  default:
    return QSortFilterProxyModel::lessThan(left, right);
  }
}

QStandardItem* PodcastService::CreateRootItem() {
  root_ = new QStandardItem(QIcon(":providers/podcast16.png"), tr("Podcasts"));
  root_->setData(true, InternetModel::Role_CanLazyLoad);
  return root_;
}

void PodcastService::LazyPopulate(QStandardItem* parent) {
  switch (parent->data(InternetModel::Role_Type).toInt()) {
  case InternetModel::Type_Service:
    PopulatePodcastList(model_->invisibleRootItem());
    model()->merged_model()->AddSubModel(parent->index(), proxy_);
    break;
  }
}

void PodcastService::PopulatePodcastList(QStandardItem* parent) {
  // Do this here since the downloader won't be created yet in the ctor.
  connect(app_->podcast_downloader(),
          SIGNAL(ProgressChanged(PodcastEpisode,PodcastDownloader::State,int)),
          SLOT(DownloadProgressChanged(PodcastEpisode,PodcastDownloader::State,int)));

  if (default_icon_.isNull()) {
    default_icon_ = QIcon(":providers/podcast16.png");
  }

  foreach (const Podcast& podcast, backend_->GetAllSubscriptions()) {
    parent->appendRow(CreatePodcastItem(podcast));
  }
}

void PodcastService::UpdatePodcastText(QStandardItem* item, int unlistened_count) const {
  const Podcast podcast = item->data(Role_Podcast).value<Podcast>();

  QString title = podcast.title();
  QFont font;

  if (unlistened_count > 0) {
    // Add the number of new episodes after the title.
    title.append(QString(" (%1)").arg(unlistened_count));

    // Set a bold font
    font.setBold(true);
  }

  item->setFont(font);
  item->setText(title);
}

void PodcastService::UpdateEpisodeText(QStandardItem* item,
                                       PodcastDownloader::State state,
                                       int percent) {
  const PodcastEpisode episode = item->data(Role_Episode).value<PodcastEpisode>();

  QString title = episode.title();
  QString tooltip;
  QFont font;
  QIcon icon;

  // Unlistened episodes are bold
  if (!episode.listened()) {
    font.setBold(true);
  }

  // Downloaded episodes get an icon
  if (episode.downloaded()) {
    if (downloaded_icon_.isNull()) {
      downloaded_icon_ = IconLoader::Load("document-save");
    }
    icon = downloaded_icon_;
  }

  // Queued or downloading episodes get icons, tooltips, and maybe a title.
  switch (state) {
  case PodcastDownloader::Queued:
    if (queued_icon_.isNull()) {
      queued_icon_ = QIcon(":icons/22x22/user-away.png");
    }
    icon    = queued_icon_;
    tooltip = tr("Download queued");
    break;

  case PodcastDownloader::Downloading:
    if (downloading_icon_.isNull()) {
      downloading_icon_ = IconLoader::Load("go-down");
    }
    icon    = downloading_icon_;
    tooltip = tr("Downloading (%1%)...").arg(percent);
    title   = QString("[ %1% ] %2").arg(QString::number(percent), episode.title());
    break;

  case PodcastDownloader::Finished:
  case PodcastDownloader::NotDownloading:
    break;
  }

  item->setFont(font);
  item->setText(title);
  item->setIcon(icon);
}

QStandardItem* PodcastService::CreatePodcastItem(const Podcast& podcast) {
  QStandardItem* item = new QStandardItem;

  // Add the episodes in this podcast and gather aggregate stats.
  int unlistened_count = 0;
  foreach (const PodcastEpisode& episode, backend_->GetEpisodes(podcast.database_id())) {
    if (!episode.listened()) {
      unlistened_count ++;
    }

    item->appendRow(CreatePodcastEpisodeItem(episode));
  }

  item->setIcon(default_icon_);
  item->setData(Type_Podcast, InternetModel::Role_Type);
  item->setData(QVariant::fromValue(podcast), Role_Podcast);
  item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
  UpdatePodcastText(item, unlistened_count);

  // Load the podcast's image if it has one
  if (podcast.ImageUrlSmall().isValid()) {
    icon_loader_->LoadIcon(podcast.ImageUrlSmall().toString(), QString(), item);
  }

  podcasts_by_database_id_[podcast.database_id()] = item;

  return item;
}

QStandardItem* PodcastService::CreatePodcastEpisodeItem(const PodcastEpisode& episode) {
  QStandardItem* item = new QStandardItem;
  item->setText(episode.title());
  item->setData(Type_Episode, InternetModel::Role_Type);
  item->setData(QVariant::fromValue(episode), Role_Episode);
  item->setData(InternetModel::PlayBehaviour_UseSongLoader, InternetModel::Role_PlayBehaviour);
  item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);

  UpdateEpisodeText(item);

  episodes_by_database_id_[episode.database_id()] = item;

  return item;
}

void PodcastService::ShowContextMenu(const QPoint& global_pos) {
  if (!context_menu_) {
    context_menu_ = new QMenu;
    context_menu_->addAction(
          IconLoader::Load("list-add"), tr("Add podcast..."),
          this, SLOT(AddPodcast()));
    context_menu_->addAction(
          IconLoader::Load("view-refresh"), tr("Update all podcasts"),
          app_->podcast_updater(), SLOT(UpdateAllPodcastsNow()));
    
    context_menu_->addSeparator();
    context_menu_->addActions(GetPlaylistActions());

    context_menu_->addSeparator();
    update_selected_action_ = context_menu_->addAction(
          IconLoader::Load("view-refresh"), tr("Update this podcast"),
          this, SLOT(UpdateSelectedPodcast()));
    download_selected_action_ = context_menu_->addAction(
          IconLoader::Load("download"), "",
          this, SLOT(DownloadSelectedEpisode()));
    delete_downloaded_action_ = context_menu_->addAction(
          IconLoader::Load("edit-delete"), tr("Delete downloaded data"),
          this, SLOT(DeleteDownloadedData()));
    remove_selected_action_ = context_menu_->addAction(
          IconLoader::Load("list-remove"), tr("Unsubscribe"),
          this, SLOT(RemoveSelectedPodcast()));

    context_menu_->addSeparator();
    set_new_action_ = context_menu_->addAction(
          tr("Mark as new"), this, SLOT(SetNew()));
    set_listened_action_ = context_menu_->addAction(
          tr("Mark as listened"), this, SLOT(SetListened()));

    context_menu_->addSeparator();
    context_menu_->addAction(
          IconLoader::Load("configure"), tr("Configure podcasts..."),
          this, SLOT(ShowConfig()));
  }

  selected_episodes_.clear();
  selected_podcasts_.clear();
  QSet<int> podcast_ids;

  foreach (const QModelIndex& index, model()->selected_indexes()) {
    switch (index.data(InternetModel::Role_Type).toInt()) {
    case Type_Podcast: {
      const int id = index.data(Role_Podcast).value<Podcast>().database_id();
      if (!podcast_ids.contains(id)) {
        selected_podcasts_.append(index);
        podcast_ids.insert(id);
      }
      break;
    }

    case Type_Episode: {
      selected_episodes_.append(index);

      // Add the parent podcast as well.
      const QModelIndex parent = index.parent();
      const int id = parent.data(Role_Podcast).value<Podcast>().database_id();
      if (!podcast_ids.contains(id)) {
        selected_podcasts_.append(parent);
        podcast_ids.insert(id);
      }
      break;
    }
    }
  }

  const bool episodes = !selected_episodes_.isEmpty();
  const bool podcasts = !selected_podcasts_.isEmpty();

  update_selected_action_->setEnabled(podcasts);
  remove_selected_action_->setEnabled(podcasts);

  if (selected_episodes_.count() == 1) {
    const PodcastEpisode episode = selected_episodes_[0].data(Role_Episode).value<PodcastEpisode>();
    const bool downloaded = episode.downloaded();
    const bool listened = episode.listened();

    download_selected_action_->setEnabled(!downloaded);
    delete_downloaded_action_->setEnabled(downloaded);
    set_new_action_->setEnabled(listened);
    set_listened_action_->setEnabled(!listened);
  } else {
    download_selected_action_->setEnabled(episodes);
    delete_downloaded_action_->setEnabled(episodes);
    set_new_action_->setEnabled(episodes);
    set_listened_action_->setEnabled(episodes);
  }

  if (selected_episodes_.count() > 1) {
    download_selected_action_->setText(tr("Download %n episodes", "", selected_episodes_.count()));
  } else {
    download_selected_action_->setText(tr("Download this episode"));
  }
  
  GetAppendToPlaylistAction()->setEnabled(episodes || podcasts);
  GetReplacePlaylistAction()->setEnabled(episodes || podcasts);
  GetOpenInNewPlaylistAction()->setEnabled(episodes || podcasts);

  context_menu_->popup(global_pos);
}

void PodcastService::UpdateSelectedPodcast() {
  foreach (const QModelIndex& index, selected_podcasts_) {
    app_->podcast_updater()->UpdatePodcastNow(
          index.data(Role_Podcast).value<Podcast>());
  }
}

void PodcastService::RemoveSelectedPodcast() {
  foreach (const QModelIndex& index, selected_podcasts_) {
    backend_->Unsubscribe(index.data(Role_Podcast).value<Podcast>());
  }
}

void PodcastService::ReloadSettings() {
  QSettings s;
  s.beginGroup(LibraryView::kSettingsGroup);

  use_pretty_covers_ = s.value("pretty_covers", true).toBool();
  // TODO: reload the podcast icons that are already loaded?
}

void PodcastService::EnsureAddPodcastDialogCreated() {
  if (!add_podcast_dialog_) {
    add_podcast_dialog_.reset(new AddPodcastDialog(app_));
  }
}

void PodcastService::AddPodcast() {
  EnsureAddPodcastDialogCreated();
  add_podcast_dialog_->show();
}

void PodcastService::SubscriptionAdded(const Podcast& podcast) {
  // Ensure the root item is lazy loaded already
  LazyLoadRoot();
  
  // The podcast might already be in the list - maybe the LazyLoadRoot() above
  // added it.
  QStandardItem* item = podcasts_by_database_id_[podcast.database_id()];
  if (!item) {
    item = CreatePodcastItem(podcast);
    model_->appendRow(item);
  }

  emit ScrollToIndex(MapToMergedModel(item->index()));
}

void PodcastService::SubscriptionRemoved(const Podcast& podcast) {
  QStandardItem* item = podcasts_by_database_id_.take(podcast.database_id());
  if (item) {
    // Remove any episode ID -> item mappings for the episodes in this podcast.
    for (int i=0 ; i<item->rowCount() ; ++i) {
      QStandardItem* episode_item = item->child(i);
      const int episode_id =
          episode_item->data(Role_Episode).value<PodcastEpisode>().database_id();

      episodes_by_database_id_.remove(episode_id);
    }

    // Remove this episode's row
    model_->removeRow(item->row());
  }
}

void PodcastService::EpisodesAdded(const QList<PodcastEpisode>& episodes) {
  QSet<int> seen_podcast_ids;

  foreach (const PodcastEpisode& episode, episodes) {
    const int database_id = episode.podcast_database_id();
    QStandardItem* parent = podcasts_by_database_id_[database_id];
    if (!parent)
      continue;

    parent->appendRow(CreatePodcastEpisodeItem(episode));

    if (!seen_podcast_ids.contains(database_id)) {
      // Update the unlistened count text once for each podcast
      int unlistened_count = 0;
      foreach (const PodcastEpisode& episode, backend_->GetEpisodes(database_id)) {
        if (!episode.listened()) {
          unlistened_count ++;
        }
      }

      UpdatePodcastText(parent, unlistened_count);
      seen_podcast_ids.insert(database_id);
    }
  }
}

void PodcastService::EpisodesUpdated(const QList<PodcastEpisode>& episodes) {
  QSet<int> seen_podcast_ids;

  foreach (const PodcastEpisode& episode, episodes) {
    const int podcast_database_id = episode.podcast_database_id();
    QStandardItem* item = episodes_by_database_id_[episode.database_id()];
    QStandardItem* parent = podcasts_by_database_id_[podcast_database_id];
    if (!item || !parent)
      continue;

    // Update the episode data on the item, and update the item's text.
    item->setData(QVariant::fromValue(episode), Role_Episode);
    UpdateEpisodeText(item);

    // Update the parent podcast's text too.
    if (!seen_podcast_ids.contains(podcast_database_id)) {
      // Update the unlistened count text once for each podcast
      int unlistened_count = 0;
      foreach (const PodcastEpisode& episode, backend_->GetEpisodes(podcast_database_id)) {
        if (!episode.listened()) {
          unlistened_count ++;
        }
      }

      UpdatePodcastText(parent, unlistened_count);
      seen_podcast_ids.insert(podcast_database_id);
    }
  }
}

void PodcastService::DownloadSelectedEpisode() {
  foreach (const QModelIndex& index, selected_episodes_) {
    app_->podcast_downloader()->DownloadEpisode(
          index.data(Role_Episode).value<PodcastEpisode>());
  }
}

void PodcastService::DeleteDownloadedData() {
  foreach (const QModelIndex& index, selected_episodes_) {
    app_->podcast_downloader()->DeleteEpisode(
          index.data(Role_Episode).value<PodcastEpisode>());
  }
}

void PodcastService::DownloadProgressChanged(const PodcastEpisode& episode,
                                             PodcastDownloader::State state,
                                             int percent) {
  QStandardItem* item = episodes_by_database_id_[episode.database_id()];
  if (!item)
    return;

  UpdateEpisodeText(item, state, percent);
}

void PodcastService::ShowConfig() {
  app_->OpenSettingsDialogAtPage(SettingsDialog::Page_Podcasts);
}

void PodcastService::CurrentSongChanged(const Song& metadata) {
  // Check whether this song is one of our podcast episodes.
  PodcastEpisode episode = backend_->GetEpisodeByUrlOrLocalUrl(metadata.url());
  if (!episode.is_valid())
    return;

  // Mark it as listened if it's not already
  if (!episode.listened()) {
    episode.set_listened(true);
    episode.set_listened_date(QDateTime::currentDateTime());
    backend_->UpdateEpisodes(PodcastEpisodeList() << episode);
  }
}

void PodcastService::SetNew() {
  SetListened(selected_episodes_, false);
}

void PodcastService::SetListened() {
  SetListened(selected_episodes_, true);
}

void PodcastService::SetListened(const QModelIndexList& indexes, bool listened) {
  PodcastEpisodeList episodes;

  QDateTime current_date_time = QDateTime::currentDateTime();

  foreach (const QModelIndex& index, indexes) {
    PodcastEpisode episode = index.data(Role_Episode).value<PodcastEpisode>();
    episode.set_listened(listened);
    if (listened) {
      episode.set_listened_date(current_date_time);
    }
    episodes << episode;
  }

  backend_->UpdateEpisodes(episodes);
}

QModelIndex PodcastService::MapToMergedModel(const QModelIndex& index) const {
  return model()->merged_model()->mapFromSource(proxy_->mapFromSource(index));
}

void PodcastService::LazyLoadRoot() {
  if (root_->data(InternetModel::Role_CanLazyLoad).toBool()) {
    root_->setData(false, InternetModel::Role_CanLazyLoad);
    LazyPopulate(root_);
  }
}

void PodcastService::SubscribeAndShow(const QVariant& podcast_or_opml) {
  if (podcast_or_opml.canConvert<Podcast>()) {
    Podcast podcast(podcast_or_opml.value<Podcast>());
    backend_->Subscribe(&podcast);

    // Lazy load the root item if it hasn't been already
    LazyLoadRoot();

    QStandardItem* item = podcasts_by_database_id_[podcast.database_id()];
    if (item) {
      // There will be an item already if this podcast was already there,
      // otherwise it'll be scrolled to when the item is created.
      emit ScrollToIndex(MapToMergedModel(item->index()));
    }
  } else if (podcast_or_opml.canConvert<OpmlContainer>()) {
    EnsureAddPodcastDialogCreated();

    add_podcast_dialog_->ShowWithOpml(podcast_or_opml.value<OpmlContainer>());
  }
}
