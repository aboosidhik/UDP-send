void SaveAvFrame(AVFrame *avFrame)
{
    FILE *fDump = fopen("...", "ab");

    uint32_t pitchY = avFrame->linesize[0];
    uint32_t pitchU = avFrame->linesize[1];
    uint32_t pitchV = avFrame->linesize[2];

    uint8_t *avY = avFrame->data[0];
    uint8_t *avU = avFrame->data[1];
    uint8_t *avV = avFrame->data[2];

    for (uint32_t i = 0; i < avFrame->height; i++) {
        fwrite(avY, avFrame->width, 1, fDump);
        avY += pitchY;
    }

    for (uint32_t i = 0; i < avFrame->height/2; i++) {
        fwrite(avU, avFrame->width/2, 1, fDump);
        avU += pitchU;
    }

    for (uint32_t i = 0; i < avFrame->height/2; i++) {
        fwrite(avV, avFrame->width/2, 1, fDump);
        avV += pitchV;
    }

    fclose(fDump);
}
