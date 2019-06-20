//
// Created by zhangzd on 2019-06-19.
//

#include <cstring>
#include "VideoChannel.h"
#include "include/x264.h"
#include "librtmp/rtmp.h"



void VideoChannel::setVideoEncInfo(int width, int height, int fps, int bitRate) {

    mWidth = width;
    mHeight = height;
    mfps = fps;
    mBitRate = bitRate;
    mYSize = width * height;
    mUvSize = mYSize / 4;

    //设置编码器信息
    x264_param_t param;
    x264_param_default_preset(&param,"ultrafast","zerolatency");
    //编码复杂度
    param.i_level_idc = 32;
    //编码格式
    param.i_csp = X264_CSP_I420;
    //设置宽高
    param.i_height = height;
    param.i_width = width;
    //设置无b帧，为实现首开
    param.i_bframe = 0;

    //set rate control params
    //设置速率控制参数

    //参数i_rc_method表示码率控制，CQP(恒定质量)，CRF(恒定码率)，ABR(平均码率)
    param.rc.i_rc_method = X264_RC_ABR;
    //码率(比特率,单位Kbps)  bitrate (bps)
    param.rc.i_bitrate = bitRate / 1000;
    //瞬时最大码率   网速   1M    10M
    param.rc.i_vbv_max_bitrate = static_cast<int>(bitRate / 1000 * 1.2);
    //设置了i_vbv_max_bitrate必须设置此参数，码率控制区大小,单位kbps
    param.rc.i_vbv_buffer_size = bitRate /1000;

    param.i_fps_num = fps;
    param.i_fps_den = 1;
    param.i_timebase_den = param.i_fps_num;
    param.i_timebase_num = param.i_fps_den;

    //1 ：时间基和时间戳用于码率控制  0 ：仅帧率用于码率控制
    param.b_vfr_input = 0;
    //关键帧间隔 2s 一个关键帧
    param.i_keyint_max = fps  * 2;

    // 是否复制sps和pps放在每个关键帧的前面 该参数设置是让每个关键帧(I帧)都附带sps/pps。
    param.b_repeat_headers = 1;
    //多线程
    param.i_threads = 1;

    //添加配置文件限定
    x264_param_apply_profile(&param,"baseline");
    //打开编码器
    videoCodec = x264_encoder_open(&param);

    pic_in = new x264_picture_t;  //此处需不需要new

    x264_picture_alloc(pic_in,X264_CSP_I420,width,height);
}

//将视频流进行编码
void VideoChannel::encodeData(int8_t* data) {

    //将nv21转换为ℹ️420
    memcpy(pic_in->img.plane[0],data,mYSize);
    for (int i = 0; i < mUvSize; ++i) {
        *(pic_in->img.plane[1] + i) = *(data + mYSize + (i * 2 + 1)); //复制u
        *(pic_in->img.plane[2] + i) = *(data + mYSize + i * 2);; //复制v
    }
    x264_nal_t *pp_nal;  //编码后的nalu数组
    int pi_nal; //编码后的nalu个数
    x264_picture_t pic_out;
    //通过x264 将i420编码成nalu格式
    x264_encoder_encode(videoCodec,&pp_nal,&pi_nal,pic_in,&pic_out);


    //将nalu转换为rtmp可传输的packet格式，进行推送

    int sps_len;
    int pps_len;
    uint8_t sps[100];
    uint8_t pps[100];
    for (int i = 0; i < pi_nal; ++i) {
        x264_nal_t nalu = pp_nal[i];
        if(nalu.i_type == NAL_SPS) {
            sps_len = nalu.i_payload - 4;
            memcpy(sps,nalu.p_payload + 4,sps_len);
        }else if(nalu.i_type == NAL_PPS) {
            pps_len = nalu.i_payload - 4;
            memcpy(pps,nalu.p_payload + 4,pps_len);
            sendSpsPps(sps, pps, sps_len, pps_len);
        }else {
            sendFrame(nalu.i_type,nalu.p_payload,nalu.i_payload);
        }
    }

}


void VideoChannel::setCallback(VideoCallback  callback) {
    videoCallback = callback;
}



//将sps,pps 封装成rtmpPacket，进行发送
void VideoChannel::sendSpsPps(uint8_t *sps, uint8_t *pps, int sps_len, int pps_len) {
    int bodySize = 13 + sps_len + 3 + pps_len;
    RTMPPacket * packet = new RTMPPacket;
    RTMPPacket_Alloc(packet,bodySize);

    int i = 0;
    packet->m_body[i++] = 0x17;
    packet->m_body[i++] = 0x00;
    packet->m_body[i++] = 0x00;
    packet->m_body[i++] = 0x00;
    packet->m_body[i++] = 0x00;

    //版本
    packet->m_body[i++] = 0x01;
//
//    //profile
    packet->m_body[i++] = sps[1];
//    //兼容性
    packet->m_body[i++] = sps[2];
//    //profile level
    packet->m_body[i++] = sps[3];
//
    packet->m_body[i++] = 0xFF;
    packet->m_body[i++] = 0xE1;
    //sps长度，两个字节
    packet->m_body[i++] = (sps_len >> 8) & 0xff;
    packet->m_body[i++] = sps_len & 0xff;
//
    memcpy(&packet->m_body[i],sps,sps_len);
//
    i += sps_len;
//    //pps个数
    packet->m_body[i++] = 0x01;
    packet->m_body[i++] = (pps_len >> 8) & 0xff;
    packet->m_body[i++] = (pps_len) & 0xff;
    memcpy(&packet->m_body[i], pps, pps_len);

    packet->m_nChannel = 10;
    packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
    packet->m_hasAbsTimestamp = 0;
    packet->m_nTimeStamp = 0;
    packet->m_nBodySize = bodySize;
    packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;

    //回调，将数据回调到native_lib，进行发送
    videoCallback(packet);
}

void VideoChannel::sendFrame(int type, uint8_t *p_payload, int i_payload) {
    if (p_payload[2] == 0x00) {
        //以00 00 00 01 分割
        p_payload += 4;
        i_payload -= 4;
    }else {
        //以00 00 01 分割
        p_payload += 3;
        i_payload -= 3;
    }

    RTMPPacket *packet = new RTMPPacket;
    int bodySize =  9 + i_payload;
    RTMPPacket_Alloc(packet,bodySize);
    packet->m_nChannel = 10;
    packet->m_nInfoField2 = RTMP_PACKET_TYPE_VIDEO;
    packet->m_hasAbsTimestamp = 0;
    packet->m_nTimeStamp = 0;
    packet->m_nBodySize = bodySize;
    packet->m_headerType = RTMP_PACKET_SIZE_LARGE;

    packet->m_body[0] = 0x27;
    if(type == NAL_SLICE_IDR){
        packet->m_body[0] = 0x17;
    }

    //类型
    packet->m_body[1] = 0x01;
    //时间戳
    packet->m_body[2] = 0x00;
    packet->m_body[3] = 0x00;
    packet->m_body[4] = 0x00;

//数据长度 int 4个字节
    packet->m_body[5] = (i_payload >> 24) & 0xff;
    packet->m_body[6] = (i_payload >> 16) & 0xff;
    packet->m_body[7] = (i_payload >> 8) & 0xff;
    packet->m_body[8] = (i_payload) & 0xff;
    //图片数据
    memcpy(&packet->m_body[9], p_payload, i_payload);
    videoCallback(packet);
}
