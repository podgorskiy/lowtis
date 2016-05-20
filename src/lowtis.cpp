#include <lowtis/lowtis.h>
#include <lowtis/BlockFetchFactory.h>
#include <vector>

using namespace lowtis;
using namespace libdvid;
using std::vector;

ImageService::ImageService(LowtisConfigPtr config_) : config(config_)
{
    fetcher = create_blockfetcher(config);
    cache.set_timer(config->refresh_rate);
    cache.set_max_size(config->cache_size);
}

void ImageService::pause()
{
    gmutex.lock();
    paused ^= 1;
    gmutex.unlock();
}

void ImageService::flush_cache()
{
    gmutex.lock();
    cache.flush();
    gmutex.unlock();
}

BinaryDataPtr ImageService::retrieve_image(unsigned int width,
        unsigned int height, vector<int> offset, int zoom)
{
    gmutex.lock(); // do I need to lock the entire function?
    
    // create image buffer
    char* bytebuffer = new char[width*height*(config->bytedepth)];

    // find intersecting blocks
    // TODO: make 2D call instead (remove dims and say dim1, dim2)
    vector<unsigned int> dims;
    dims.push_back(width);
    dims.push_back(height);
    dims.push_back(1);
    vector<DVIDCompressedBlock> blocks = fetcher->intersecting_blocks(dims, offset);

    // check cache and save missing blocks
    vector<DVIDCompressedBlock> current_blocks;
    vector<DVIDCompressedBlock> missing_blocks;

    for (auto iter = blocks.begin(); iter != blocks.end(); ++iter) {
        BlockCoords coords;
        vector<int> toffset = iter->get_offset();
        coords.x = toffset[0];
        coords.y = toffset[1];
        coords.z = toffset[2];

        DVIDCompressedBlock block = *iter;
        bool found = cache.retrieve_block(coords, block);
        if (found) {
            current_blocks.push_back(block);
        } else {
            missing_blocks.push_back(block);
        }
    }

    // call interface for blocks desired 
    fetcher->extract_specific_blocks(missing_blocks);
    
    // concatenate block lists
    current_blocks.insert(current_blocks.end(), missing_blocks.begin(), 
            missing_blocks.end());

    // populate image from blocks and return data
    for (auto iter = current_blocks.begin(); iter != current_blocks.end(); ++iter) {
        bool emptyblock = false;
        if (!(iter->get_data())) {
            emptyblock = true;
        }
        
        size_t blocksize = iter->get_blocksize();

        const unsigned char* raw_data = 0;

        if (!emptyblock) {
            raw_data = iter->get_uncompressed_data()->get_raw();
        }

        // extract common dim3 offset (will refer to as 'z')
        vector<int> toffset = iter->get_offset();
        int zoff = offset[2] - toffset[2];

        // find intersection between block and buffer
        int startx = std::max(offset[0], toffset[0]);
        int finishx = std::min(offset[0]+blocksize, toffset[0]+blocksize);
        int starty = std::max(offset[1], toffset[1]);
        int finishy = std::min(offset[1]+blocksize, toffset[1]+blocksize);

        unsigned long long iterpos = (zoff - toffset[2]) * blocksize * blocksize * config->bytedepth;

        // point to correct plane
        raw_data += iterpos;
        
        // point to correct y,x
        raw_data += (((toffset[1]-starty)*blocksize*config->bytedepth) + 
                ((toffset[0]-startx)*config->bytedepth)); 
        char* bytebuffer_temp = bytebuffer + ((offset[1]-starty)*width*config->bytedepth) +
           ((offset[0]-startx)*config->bytedepth); 

        for (int ypos = starty; ypos < finishy; ++ypos) {
            for (int xpos = startx; xpos < finishx; ++xpos) {
                for (int bytepos = 0; bytepos < config->bytedepth; ++bytepos) {
                    if (!emptyblock) {
                        *bytebuffer_temp = *raw_data;
                    } else {
                        *bytebuffer_temp = config->emptyval;
                    }
                    ++raw_data;
                    ++bytebuffer_temp;
                } 
            }
            raw_data += (blocksize-(finishx-startx))*config->bytedepth;
            bytebuffer_temp += (width-(finishx-startx))*config->bytedepth;
        }
    }
    gmutex.unlock();

    // TODO: avoid extra mem copy
    libdvid::BinaryDataPtr data = libdvid::BinaryData::create_binary_data(bytebuffer, width*height*config->bytedepth);
    delete []bytebuffer;

    return data;
}