//
// Created by cpasjuste on 13/11/18.
//

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
int ffmpeg_main(int argc, const char **argv);
}

#include "main.h"
#include "media_thread.h"
#include "base64.h"
#include "utility.h"

#if 0
static void dump_metadata(const std::string &desc, AVDictionary *dic) {
    AVDictionaryEntry *t = nullptr;
    printf("- %s\n", desc.c_str());
    while ((t = av_dict_get(dic, "", t, AV_DICT_IGNORE_SUFFIX))) {
        printf("\t%s -> %s\n", t->key, t->value);
    }
}
#endif

static int media_info_thread(void *ptr) {

    auto *mediaThread = (MediaThread *) ptr;

    printf("media_info_thread: start\n");

    while (mediaThread->isRunning()) {

        if (!mediaThread->isRunning()) {
            break;
        }

        //if (mediaThread->getMain()->getPlayer() && mediaThread->getMain()->getPlayer()->isPlaying()) {
        //    mediaThread->getMain()->delay(100);
        //    continue;
        //}

        if (mediaThread->mediaList.empty()) {
            mediaThread->getMain()->delay(100);
            continue;
        }

        std::string mediaPath = mediaThread->mediaList[0];
        std::string cachePath = mediaThread->getMediaCachePath(mediaPath);
        printf("media_info_thread: process: %s => %s\n", mediaPath.c_str(), cachePath.c_str());

        // open
        AVFormatContext *ctx = nullptr;
        int res = avformat_open_input(&ctx, mediaPath.c_str(), nullptr, nullptr);
        if (res != 0) {
            char err_str[256];
            av_strerror(res, err_str, 255);
            printf("media_info_thread: unable to open '%s': %s\n", mediaPath.c_str(), err_str);
            // cache an "unknow" media file so we don't try that file again
            MediaInfo media;
            media.serialize(cachePath);
            continue;
        }

        av_opt_set_int(ctx, "probesize", INT_MAX, 0);
        av_opt_set_int(ctx, "analyzeduration", INT_MAX, 0);
        res = avformat_find_stream_info(ctx, nullptr);
        if (res < 0) {
            char err_str[256];
            av_strerror(res, err_str, 255);
            printf("media_info_thread: unable to parse '%s': %s\n", mediaPath.c_str(), err_str);
            avformat_close_input(&ctx);
            // cache an "unknow" media file so we don't try that file again
            MediaInfo media;
            media.serialize(cachePath);
            continue;
        }

        MediaInfo media;
        AVDictionaryEntry *language, *title = av_dict_get(ctx->metadata, "title", nullptr, 0);
        media.title = title ? title->value : "Unknown";
        media.path = mediaPath;
        media.duration = ctx->duration / AV_TIME_BASE;
        printf("media_info_thread: stream count: %i\n", ctx->nb_streams);
        //dump_metadata("media", ctx->metadata);

        for (int i = 0; i < (int) ctx->nb_streams; i++) {
            const AVCodecParameters *codec = ctx->streams[i]->codecpar;
            int type = codec->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO) {
                MediaInfo::Stream stream{};
                title = av_dict_get(ctx->streams[i]->metadata, "title", nullptr, 0);
                language = av_dict_get(ctx->streams[i]->metadata, "language", nullptr, 0);
                stream.title = title ? title->value : "Unknown";
                stream.language = language ? language->value : "";
                stream.codec = avcodec_get_name(codec->codec_id);
                stream.rate = (int) ctx->bit_rate;
                stream.width = codec->width;
                stream.height = codec->height;
                media.videos.push_back(stream);
                printf("media_info_thread: found video stream: %s\n", stream.title.c_str());
                //dump_metadata("video stream", ctx->streams[i]->metadata);
            } else if (type == AVMEDIA_TYPE_AUDIO) {
                MediaInfo::Stream stream{};
                title = av_dict_get(ctx->streams[i]->metadata, "title", nullptr, 0);
                language = av_dict_get(ctx->streams[i]->metadata, "language", nullptr, 0);
                stream.title = title ? title->value : "Unknown";
                stream.language = language ? language->value : "";
                stream.codec = avcodec_get_name(codec->codec_id);
                stream.rate = codec->sample_rate;
                media.audios.push_back(stream);
                printf("media_info_thread: found audio stream: %s\n", stream.title.c_str());
                //dump_metadata("audio stream", ctx->streams[i]->metadata);
            } else if (type == AVMEDIA_TYPE_SUBTITLE) {
                MediaInfo::Stream stream{};
                title = av_dict_get(ctx->streams[i]->metadata, "title", nullptr, 0);
                language = av_dict_get(ctx->streams[i]->metadata, "language", nullptr, 0);
                stream.title = title ? title->value : "Unknown";
                stream.language = language ? language->value : "";
                stream.codec = avcodec_get_name(codec->codec_id);
                media.subtitles.push_back(stream);
                printf("media_info_thread: found subtitle stream: %s\n", stream.title.c_str());
                //dump_metadata("subtitle stream", ctx->streams[i]->metadata);
            }
        }

        media.serialize(cachePath);

        // close
        avformat_close_input(&ctx);

        // remove from list
        SDL_LockMutex(mediaThread->getMutex());
        mediaThread->mediaList.erase(mediaThread->mediaList.begin());
        SDL_UnlockMutex(mediaThread->getMutex());

#if 0
        // TODO: extract thumbnail
        std::string p = cachePath + ".png";
        const char *argv[] = {
                "ffmpeg", "-i", mediaPath.c_str(), "-ss", "00:00:02", "-vframes", "1", p.c_str()
        };
        ffmpeg_main(8, argv);
#endif
        printf("media_info_thread: process: OK\n");
    }

    printf("media_info_thread: end\n");

    return 0;
}

MediaThread::MediaThread(Main *main, const std::string &cachePath) {

    this->main = main;
    this->cache = cachePath;

    avformat_network_init();

    mutex = SDL_CreateMutex();
    thread = SDL_CreateThread(media_info_thread, "pplay_info", (void *) this);
}

const MediaInfo MediaThread::getMediaInfo(const c2d::Io::File &file) {

    MediaInfo media;

    if (pplay::Utility::isMedia(file)) {
        std::string cachePath = getMediaCachePath(file.path);
        if (main->getIo()->exist(cachePath)) {
            //printf("getMediaInfo::deserialize: %s\n", file.name.c_str());
            media.deserialize(cachePath);
        } else {
            // media info not yet available, cache for later use
            SDL_LockMutex(mutex);
            if (std::find(mediaList.begin(), mediaList.end(), file.path) == mediaList.end()) {
                mediaList.emplace_back(file.path);
            } else {
                //printf("getMediaInfo: media info in process: %s\n", file.name.c_str());
            }
            SDL_UnlockMutex(mutex);
        }
    }

    return media;
}

void MediaThread::cacheDir(const std::string &dir) {

    SDL_LockMutex(mutex);

    std::vector<c2d::Io::File> files = main->getIo()->getDirList(dir, true);
    for (c2d::Io::File &file : files) {
        if (pplay::Utility::isMedia(file)) {
            std::string cachePath = getMediaCachePath(file.path);
            if (!main->getIo()->exist(cachePath)) {
                if (std::find(mediaList.begin(), mediaList.end(), file.path) == mediaList.end()) {
                    mediaList.emplace_back(file.path);
                }
            }
        }
    }

    SDL_UnlockMutex(mutex);
}

Main *MediaThread::getMain() {
    return main;
}

const std::string MediaThread::getMediaCachePath(const std::string &mediaPath) const {
    std::string path = cache +
                       base64_encode((const unsigned char *) mediaPath.c_str(),
                                     (unsigned int) mediaPath.length());
    return path;
}

SDL_mutex *MediaThread::getMutex() {
    return mutex;
}

bool MediaThread::isRunning() {
    return running;
}

MediaThread::~MediaThread() {

    int ret;

    running = false;
    printf("~Media: wait thread...\n");
    SDL_WaitThread(thread, &ret);
    //SDL_DetachThread(thread);
    SDL_DestroyMutex(mutex);
    printf("~Media: done...\n");

    avformat_network_deinit();
}
