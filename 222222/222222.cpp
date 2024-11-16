#include <SDL2/SDL_thread.h>
#include <SDL2/SDL_mutex.h>
#include <SDL2/SDL_audio.h>
#include <SDL2/SDL.h>
#include <windows.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
#include <libavutil/frame.h>
#include <libavutil/channel_layout.h>
#include <libavutil/rational.h>
#include <libavutil/error.h>
}
#ifndef AV_ERROR_MAX_STRING_SIZE
#define AV_ERROR_MAX_STRING_SIZE 2048
#endif
char av_error[AV_ERROR_MAX_STRING_SIZE] = { 0 };
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

#define MAX_VIDEO_PIC_NUM 4  //最大缓存解码图片数
int frame_number = 0;
//队列
typedef struct PacketQueue {
    AVPacketList* first_pkt, * last_pkt;
    int nb_packets;
    SDL_mutex* mutex;
} PacketQueue;

//音视频同步时钟模式
enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

static  Uint8* audio_chunk;
static  Uint32  audio_len;
static  Uint8* audio_pos;

SDL_Window* sdlWindow = nullptr;
SDL_Renderer* sdlRender = nullptr;
SDL_Texture* sdlTtexture = nullptr;


AVFormatContext* ifmt_ctx = NULL;
AVPacket* pkt;
AVFrame* video_frame, * audio_frame;
int ret;
int videoindex = -1, audioindex = -1;
const char* in_filename = "1.mp4";

int frame_width = 1080;
int frame_height = 606;

//视频解码
const AVCodec* video_codec = nullptr;
AVCodecContext* video_codecContent = nullptr;

typedef struct video_pic
{
    AVFrame frame;

    float clock; //显示时钟
    float duration; //持续时间
    int frame_NUM; //帧号
}video_pic;

video_pic v_pic[MAX_VIDEO_PIC_NUM]; //视频解码最多保存四帧数据
int pic_count = 0; //以存储图片数量

//音频解码
const AVCodec* audio_codec = nullptr;
AVCodecContext* audio_codecContent = nullptr;

//视频帧队列
PacketQueue video_pkt_queue;
PacketQueue audio_pkt_queue;

bool isVideoPlaying = true; // 初始状态为播放
bool isAudioPlaying = true; // 初始状态为播放

//同步时钟设置为音频为主时钟
int av_sync_type = AV_SYNC_AUDIO_MASTER;

int64_t audio_callback_time;
float audio_clock;
float video_clock;
// 目标帧率
float target_frame_rate = 30.0;
// 每帧的时间间隔（以秒为单位）
float frame_interval = 1.0 / target_frame_rate;
//SDL音频参数结构
SDL_AudioSpec wanted_spec;

int initSdl();
void closeSDL();
void  fill_audio_pcm2(void* udata, Uint8* stream, int len);

//fltp转为packed形式
void fltp_convert_to_f32le(float* f32le, float* fltp_l, float* fltp_r, int nb_samples, int channels)
{
    for (int i = 0; i < nb_samples; i++)
    {
        f32le[i * channels] = fltp_l[i];
        f32le[i * channels + 1] = fltp_r[i];
    }
}

//将一个压缩数据包放入相应的队列中
void put_AVPacket_into_queue(PacketQueue* q, AVPacket* packet)
{
    SDL_LockMutex(q->mutex);
    AVPacketList* temp = nullptr;
    temp = (AVPacketList*)av_malloc(sizeof(AVPacketList));
    if (!temp)
    {
        printf("malloc a AVPacketList error\n");
        return;
    }

    temp->pkt = *packet;
    temp->next = NULL;

    if (!q->last_pkt)
        q->first_pkt = temp;
    else
        q->last_pkt->next = temp;

    q->last_pkt = temp;
    q->nb_packets++;

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_get(PacketQueue* q, AVPacket* pkt2)
{
    while (true)
    {
        AVPacketList* pkt1;
        //一直取，直到队列中有数据，就返回
        pkt1 = q->first_pkt;
        if (pkt1)
        {
            SDL_LockMutex(q->mutex);
            q->first_pkt = pkt1->next;

            if (!q->first_pkt)
                q->last_pkt = NULL;

            q->nb_packets--;
            SDL_UnlockMutex(q->mutex);

            *pkt2 = pkt1->pkt;

            av_free(pkt1);


            break;

        }
        else
            SDL_Delay(1);

    }

    return;
}

int delCunt = 0;
int64_t audio_pts = AV_NOPTS_VALUE;
int64_t video_pts = AV_NOPTS_VALUE;
float current_frame_time = 0;
int video_play_thread(void* data) {
    AVPacket video_packet = { 0 };
    while (true) {
        if (!isVideoPlaying) { // 检查播放状态
            SDL_Delay(100); // 暂停时，减少CPU占用
            continue; // 跳过当前循环，不处理音频帧
        }
        packet_queue_get(&video_pkt_queue, &video_packet);

        ret = avcodec_send_packet(video_codecContent, &video_packet);
        if (ret < 0) {
            fprintf(stderr, "Error sending a packet for decoding\n");
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(video_codecContent, video_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                break;
            }

            frame_number++;

            // 计算当前帧的时间（秒）
            current_frame_time = (double)video_frame->pts / (double)video_codecContent->time_base.den;
            printf("video_frame->pts:  %lld, time_base: %d/%d, current_frame_time: %f\n", video_frame->pts, video_codecContent->time_base.num, video_codecContent->time_base.den, current_frame_time);
            video_pts = current_frame_time;

            // 计算下一帧应该渲染的时间
            float next_frame_time = current_frame_time + frame_interval;

            // 获取当前音频时钟时间
            float audio_clock_current = audio_clock/1000;

            // 计算延迟，确保视频帧等待直到音频帧到达
            float delay = next_frame_time - audio_clock_current;
            printf("current_frame_time= %f  frame_interval = %f   audio_clock_current = %f        delay = %f  ", current_frame_time, frame_interval, audio_clock_current,delay);
            if (delay > 0) {
                SDL_Delay((unsigned int)(delay * 1000));
            }
            else if (delay < 0) {
                SDL_Delay(1);
            }

            // 将YUV数据更新到纹理中
            SDL_UpdateYUVTexture(sdlTtexture, nullptr, video_frame->data[0], video_frame->linesize[0], video_frame->data[1], video_frame->linesize[1], video_frame->data[2], video_frame->linesize[2]);
            // 设置渲染器的背景颜色为黑色
            SDL_SetRenderDrawColor(sdlRender, 0, 0, 0, 255); // R, G, B, A
            // 清理渲染器缓冲区
            SDL_RenderClear(sdlRender);
            // 将纹理拷贝到窗口渲染平面上
            SDL_RenderCopy(sdlRender, sdlTtexture, NULL, nullptr);
            // 翻转缓冲区，前台显示
            SDL_RenderPresent(sdlRender);

            // 更新视频时钟
            video_clock = current_frame_time;
        }
    }
}
int audio_play_thread(void* data)
{
    AVPacket audio_packt = { 0 };

    while (true)
    {
        if (!isAudioPlaying) {
            SDL_Delay(100);
            continue;
        }
        packet_queue_get(&audio_pkt_queue, &audio_packt);

        ret = avcodec_send_packet(audio_codecContent, &audio_packt);
        if (ret < 0) {
            fprintf(stderr, "Error submitting the packet to the decoder\n");
            exit(1);
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(audio_codecContent, audio_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            else if (ret < 0) {
                fprintf(stderr, "Error during decoding\n");
                break;
            }

            float current_audio_time = (double)audio_frame->pts * av_q2d(audio_codecContent->time_base);
            // 根据视频快进后的时间戳来调整音频播放
            if (current_audio_time < video_pts)
            {
                continue;
            }
            audio_pts = current_audio_time;

            int data_size = av_get_bytes_per_sample(audio_codecContent->sample_fmt);
            if (data_size < 0) {
                fprintf(stderr, "Failed to calculate data size\n");
                break;
            }

            int pcm_buffer_size = data_size * audio_frame->nb_samples * audio_codecContent->ch_layout.nb_channels;
            uint8_t* pcm_buffer = (uint8_t*)malloc(pcm_buffer_size);
            memset(pcm_buffer, 0, pcm_buffer_size);

            fltp_convert_to_f32le((float*)pcm_buffer,
                (float*)audio_frame->data[0], (float*)audio_frame->data[1],
                audio_frame->nb_samples, audio_codecContent->ch_layout.nb_channels);

            audio_chunk = pcm_buffer;
            audio_len = pcm_buffer_size;
            audio_pos = audio_chunk;

            audio_clock = audio_frame->pts * av_q2d(audio_codecContent->time_base) * 1000;

            while (audio_len > 0)
                SDL_Delay(1);

            free(pcm_buffer);
        }
    }

    return 0;
}

int open_file_thread(void* data)
{
    //读取
    while (av_read_frame(ifmt_ctx, pkt) >= 0)
    {
        if (pkt->stream_index == videoindex) {

            //加入视频队列
            put_AVPacket_into_queue(&video_pkt_queue, pkt);
           
        }
        else if (pkt->stream_index == audioindex)
        {
            //加入音频队列
            put_AVPacket_into_queue(&audio_pkt_queue, pkt);
            
        }
        else
            av_packet_unref(pkt);
        printf("Audio PTS: %lld, Video PTS: %lld, current_frame_time: %f\n", audio_pts, video_pts,current_frame_time);
    }

    return 0;
}
void fastForwardVideo(float seconds)
{
    int frames_to_skip = static_cast<int>(target_frame_rate * seconds);
    // 暂时停止视频播放线程和音频播放线程
    isVideoPlaying = false;
    isAudioPlaying = false;
    // 调整视频播放时间戳或者跳过帧
    for (int i = 0; i < frames_to_skip; ++i)
    {
        AVPacket video_packet = { 0 };
        packet_queue_get(&video_pkt_queue, &video_packet);
        av_packet_unref(&video_packet);
    }
    // 更新视频时间戳相关变量，这里假设当前帧时间戳为video_pts
    video_pts += seconds;
    // 重新开始视频播放线程和音频播放线程
    isVideoPlaying = true;
    isAudioPlaying = true;
}
void rewindVideo(float seconds) {
    int frames_to_rewind = static_cast<int>(target_frame_rate * seconds);
    // 暂时停止视频播放线程和音频播放线程
    isVideoPlaying = false;
    isAudioPlaying = false;
    // 调整视频播放时间戳或者跳过帧
    for (int i = 0; i < frames_to_rewind; ++i) {
        AVPacket video_packet = { 0 };
        // 这里需要确保队列中有足够的包可以移除
        if (video_pkt_queue.nb_packets > 0) {
            packet_queue_get(&video_pkt_queue, &video_packet);
            av_packet_unref(&video_packet);
        }
    }
    // 更新视频时间戳相关变量，这里假设当前帧时间戳为video_pts
    video_pts -= seconds;
    // 重新开始视频播放线程和音频播放线程
    isVideoPlaying = true;
    isAudioPlaying = true;
}
int main(int argc, char* argv[])
{
    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
        printf("Could not open input file: %s\n", av_err2str(ret));
        return -1;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        printf("Failed to retrieve input stream information: %s\n", av_err2str(ret));
        return -1;
    }

    videoindex = -1;
    //int video_stream_index = -1;
    for (int i = 0; i < ifmt_ctx->nb_streams; i++) { //nb_streams：视音频流的个数
        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoindex = i;
            //video_stream_index = i;
        }
        else if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            audioindex = i;
    }
    // 获取视频流的时间基准
    AVStream* video_stream = ifmt_ctx->streams[videoindex];
    AVRational video_time_base = video_stream->time_base;
    video_codecContent = avcodec_alloc_context3(video_codec);
    // 设置视频解码器上下文的时间基准
    video_codecContent->time_base = video_time_base;

    // 打印时间基准
    printf("Video time base: %d/%d\n", video_time_base.num, video_time_base.den);
    if (videoindex == -1 || audioindex == -1) {
        fprintf(stderr, "Could not find audio or video stream in the input file\n");
        return -1;
    }
    // 根据编解码器id查询解码器并返回解码器结构
    audio_codec = avcodec_find_decoder(ifmt_ctx->streams[audioindex]->codecpar->codec_id);
    if (!audio_codec) {
        fprintf(stderr, "Failed to find audio codec.\n");
        return -1;
    }

    // 分配解码器上下文
    audio_codecContent = avcodec_alloc_context3(audio_codec);
    if (!audio_codecContent) {
        fprintf(stderr, "Failed to allocate audio codec context.\n");
        return -1;
    }

    // 拷贝音频流信息到音频编码器上下文中
    if (avcodec_parameters_to_context(audio_codecContent, ifmt_ctx->streams[audioindex]->codecpar) < 0) {
        fprintf(stderr, "Failed to copy audio codec parameters to decoder context.\n");
        avcodec_free_context(&audio_codecContent);
        return -1;
    }

    // 打开音频解码器和关联解码器上下文
    if (avcodec_open2(audio_codecContent, audio_codec, nullptr) < 0) {
        fprintf(stderr, "Could not open audio codec!\n");
        avcodec_free_context(&audio_codecContent);
        return -1;
    }
    printf("\nInput Video===========================\n");
    av_dump_format(ifmt_ctx, 0, in_filename, 0);  // 打印信息
    printf("\n======================================\n");

    // 根据编解码器id查询解码器并返回解码器结构
    video_codec = avcodec_find_decoder(ifmt_ctx->streams[videoindex]->codecpar->codec_id);
    if (!video_codec) {
        printf("Failed to find video codec.\n");
        return -1;
    }

    // 分配解码器上下文
    //video_codecContent = avcodec_alloc_context3(video_codec);
    if (!video_codecContent) {
        printf("Failed to allocate video codec context.\n");
        return -1;
    }

    // 拷贝视频流信息到视频编码器上下文中
    if (avcodec_parameters_to_context(video_codecContent, ifmt_ctx->streams[videoindex]->codecpar) < 0) {
        printf("Failed to copy video codec parameters to decoder context.\n");
        return -1;
    }

    // 打开视频解码器和关联解码器上下文
    if (avcodec_open2(video_codecContent, video_codec, nullptr) < 0) {
        printf("Could not open video codec!\n");
        avcodec_free_context(&video_codecContent);
        return -1;
    }

    //申请一个AVPacket结构
    pkt = av_packet_alloc();

    //申请一个AVFrame 结构用来存放解码后的数据
    video_frame = av_frame_alloc();
    audio_frame = av_frame_alloc();

    //初始化SDL
    if (!initSdl()) {
        printf("Failed to initialize SDL.\n");
        avcodec_free_context(&video_codecContent);
        return -1;
    }

    video_pkt_queue.mutex = SDL_CreateMutex();
    audio_pkt_queue.mutex = SDL_CreateMutex();

    //设置SDL音频播放参数
    wanted_spec.freq = 44100; //采样率
    wanted_spec.format = AUDIO_F32LSB; // audio_codecContent->sample_fmt;  这里需要转换为sdl的采样格式
    wanted_spec.channels = 2; //通道数
    wanted_spec.silence = 0;
    wanted_spec.samples = 1024;   //每一帧的采样点数量
    wanted_spec.callback = fill_audio_pcm2; //音频播放回调

    //打开系统音频设备
    if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
        printf("can't open audio: %s\n", SDL_GetError());
        closeSDL();
        avcodec_free_context(&video_codecContent);
        return -1;
    }
    //Play
    SDL_PauseAudio(0);

    SDL_CreateThread(open_file_thread, "open_file", nullptr);
    SDL_CreateThread(video_play_thread, "video_play", nullptr);
    SDL_CreateThread(audio_play_thread, "audio_play", nullptr);

    bool quit = false;
    SDL_Event e;
    while (quit == false)
    {
        while (SDL_PollEvent(&e) != 0)
        {
            if (e.type == SDL_QUIT)
            {
                quit = true;
                break;
            }
            else if (e.type == SDL_KEYDOWN)
            {
                if (e.key.keysym.sym == SDLK_SPACE)
                {
                    isVideoPlaying = !isVideoPlaying;
                    isAudioPlaying = !isAudioPlaying;
                }
                else if (e.key.keysym.sym == SDLK_RIGHT)
                {
                    // 调用快进函数，快进5秒
                    fastForwardVideo(5.0);
                }
                else if (e.key.keysym.sym == SDLK_LEFT)
                {
                    rewindVideo(3.0);
                  
                }
                else if (e.key.keysym.sym == SDLK_UP)
                {
                    //adjustVolume(5);

                }
                else if (e.key.keysym.sym == SDLK_DOWN)
                {
                   
                }
            }
        }
    }

    SDL_DestroyMutex(video_pkt_queue.mutex);
    SDL_DestroyMutex(audio_pkt_queue.mutex);

    //释放ffmpeg指针
    avformat_close_input(&ifmt_ctx);
    avcodec_free_context(&video_codecContent);
    avcodec_free_context(&audio_codecContent);
    av_frame_free(&audio_frame);
    av_frame_free(&video_frame);
    av_packet_free(&pkt);
    return 0;
}
//sdl初始化
int initSdl()
{
    bool success = true;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO))
    {
        printf("init sdl error:%s\n", SDL_GetError());
        success = false;
    }

    //创建window
    sdlWindow = SDL_CreateWindow("decode video", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, frame_width, frame_height, SDL_WINDOW_SHOWN);
    if (sdlWindow == nullptr)
    {
        printf("create window error: %s\n", SDL_GetError());
        success = false;
    }

    //创建渲染器
    sdlRender = SDL_CreateRenderer(sdlWindow, -1, 0);
    if (sdlRender == nullptr)
    {
        printf("init window Render error: %s\n", SDL_GetError());
        success = false;
    }

    //构建合适的纹理
    sdlTtexture = SDL_CreateTexture(sdlRender, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame_width, frame_height);

    return success;
}

static int64_t last_callback_time = 0;

void fill_audio_pcm2(void* udata, Uint8* stream, int len) {
    int64_t current_callback_time = av_gettime();

    if (last_callback_time == 0) {
        last_callback_time = current_callback_time;
    }
    else {
        int64_t time_diff = current_callback_time - last_callback_time;
        audio_clock += time_diff / 1000000.0; // 转换为秒
        last_callback_time = current_callback_time;
    }

    SDL_memset(stream, 0, len);

    if (audio_len == 0)
        return;
    len = (len > audio_len ? audio_len : len);

    SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME);
    audio_len -= len;
    audio_pos += len;
}
void closeSDL()
{
    SDL_CloseAudio();
    SDL_DestroyWindow(sdlWindow);
    sdlWindow = nullptr;
    SDL_DestroyRenderer(sdlRender);
    sdlRender = nullptr;
    SDL_DestroyTexture(sdlTtexture);
    sdlTtexture = nullptr;
}
