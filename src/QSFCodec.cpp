/*
 *      Copyright (C) 2014 Arne Morten Kvarving
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <kodi/addon-instance/AudioDecoder.h>
#include <kodi/Filesystem.h>
#include "RingBuffer.h"

#include <algorithm>
#include <iostream>

extern "C" {
#include "qsound.h"
#include "psflib.h"

static void * psf_file_fopen( const char * uri )
{
  kodi::vfs::CFile* file = new kodi::vfs::CFile;
  if (!file->OpenFile(uri, 0))
  {
    delete file;
    return nullptr;
  }

  return file;
}

static size_t psf_file_fread( void * buffer, size_t size, size_t count, void * handle )
{
  kodi::vfs::CFile* file = static_cast<kodi::vfs::CFile*>(handle);
  return file->Read(buffer, size*count);
}

static int psf_file_fseek( void * handle, int64_t offset, int whence )
{
  kodi::vfs::CFile* file = static_cast<kodi::vfs::CFile*>(handle);
  return file->Seek(offset, whence) > -1 ? 0 : -1;
}

static int psf_file_fclose( void * handle )
{
  delete static_cast<kodi::vfs::CFile*>(handle);

  return 0;
}

static long psf_file_ftell( void * handle )
{
  kodi::vfs::CFile* file = static_cast<kodi::vfs::CFile*>(handle);
  return file->GetPosition();
}

const psf_file_callbacks psf_file_system =
{
  "\\/",
  psf_file_fopen,
  psf_file_fread,
  psf_file_fseek,
  psf_file_fclose,
  psf_file_ftell
};

}

class qsound_rom
{
  public:
    struct valid_range
    {
      uint32_t start;
      uint32_t size;
    };

    std::vector<uint8_t> m_aKey;
    std::vector<valid_range> m_aKeyValid;
    std::vector<uint8_t> m_aZ80ROM;
    std::vector<valid_range> m_aZ80ROMValid;
    std::vector<uint8_t> m_aSampleROM;
    std::vector<valid_range> m_aSampleROMValid;

    qsound_rom() {}

    void superimpose_from( const qsound_rom & from )
    {
      superimpose_section_from("KEY", from.m_aKey      , from.m_aKeyValid      );
      superimpose_section_from("Z80", from.m_aZ80ROM   , from.m_aZ80ROMValid   );
      superimpose_section_from("SMP", from.m_aSampleROM, from.m_aSampleROMValid);
    }

    void upload_section( const char * section, uint32_t start, const uint8_t * data, uint32_t size )
    {
      std::vector<uint8_t> * pArray = NULL;
      std::vector<valid_range> * pArrayValid = NULL;
      uint32_t maxsize = 0x7FFFFFFF;

      if ( !strcmp( section, "KEY" ) ) { pArray = &m_aKey; pArrayValid = &m_aKeyValid; maxsize = 11; }
      else if ( !strcmp( section, "Z80" ) ) { pArray = &m_aZ80ROM; pArrayValid = &m_aZ80ROMValid; }
      else if ( !strcmp( section, "SMP" ) ) { pArray = &m_aSampleROM; pArrayValid = &m_aSampleROMValid; }
      else return;

      if ( ( start + size ) < start )
      {
        return;
      }

      uint32_t newsize = start + size;
      uint32_t oldsize = pArray->size();
      if ( newsize > maxsize )
      {
        return;
      }

      if ( newsize > oldsize )
        pArray->resize( newsize );

      memcpy(&(*pArray)[start], data, size);

      oldsize = pArrayValid->size();
      pArrayValid->resize( oldsize + 1 );
      pArrayValid->back().start = start;
      pArrayValid->back().size = size;
    }

    void clear()
    {
      m_aKey.resize(0);
      m_aKeyValid.resize(0);
      m_aZ80ROM.resize(0);
      m_aZ80ROMValid.resize(0);
      m_aSampleROM.resize(0);
      m_aSampleROMValid.resize(0);
    }

  private:
    void superimpose_section_from(const char * section,
                                  const std::vector<uint8_t> & from,
                                  const std::vector<valid_range> & fromvalid )
    {
      for ( unsigned i = 0; i < fromvalid.size(); i++ )
      {
        const valid_range & range = fromvalid[ i ];
        uint32_t start = range.start;
        uint32_t size = range.size;
        if ( ( start >= from.size() ) ||
            ( size >= from.size() ) ||
            ( ( start + size ) > from.size() ) )
        {
          return;
        }

        upload_section( section, start, &from[start], size );
      }
    }
};


struct QSFContext
{
  qsound_rom rom;
  int64_t length;
  int sample_rate;
  int64_t pos;
  int year;
  std::string file;
  std::vector<uint8_t> qsound_state;
  CRingBuffer sample_buffer;
  std::string title;
  std::string album;
};


#define BORK_TIME 0xC0CAC01A
static unsigned long parse_time_crap(const char *input)
{
  if (!input) return BORK_TIME;
  int len = strlen(input);
  if (!len) return BORK_TIME;
  int value = 0;
  {
    int i;
    for (i = len - 1; i >= 0; i--)
    {
      if ((input[i] < '0' || input[i] > '9') && input[i] != ':' && input[i] != ',' && input[i] != '.')
      {
        return BORK_TIME;
      }
    }
  }
  std::string foo = input;
  char *bar = (char *) &foo[0];
  char *strs = bar + foo.size() - 1;
  while (strs > bar && (*strs >= '0' && *strs <= '9'))
  {
    strs--;
  }
  if (*strs == '.' || *strs == ',')
  {
    // fraction of a second
    strs++;
    if (strlen(strs) > 3) strs[3] = 0;
    value = atoi(strs);
    switch (strlen(strs))
    {
      case 1:
        value *= 100;
        break;
      case 2:
        value *= 10;
        break;
    }
    strs--;
    *strs = 0;
    strs--;
  }
  while (strs > bar && (*strs >= '0' && *strs <= '9'))
  {
    strs--;
  }
  // seconds
  if (*strs < '0' || *strs > '9') strs++;
  value += atoi(strs) * 1000;
  if (strs > bar)
  {
    strs--;
    *strs = 0;
    strs--;
    while (strs > bar && (*strs >= '0' && *strs <= '9'))
    {
      strs--;
    }
    if (*strs < '0' || *strs > '9') strs++;
    value += atoi(strs) * 60000;
    if (strs > bar)
    {
      strs--;
      *strs = 0;
      strs--;
      while (strs > bar && (*strs >= '0' && *strs <= '9'))
      {
        strs--;
      }
      value += atoi(strs) * 3600000;
    }
  }
  return value;
}

static int psf_info_meta(void * context, const char * name, const char * value)
{
  QSFContext* qsf = (QSFContext*)context;

  if (!strcasecmp(name, "game"))
    qsf->album = value;
  else if (!strcasecmp(name, "year"))
    qsf->year = atoi(value);
  else if (!strcasecmp(name, "length"))
  {
    int temp = parse_time_crap(value);
    if (temp != BORK_TIME)
      qsf->length = temp;
  }

  return 0;
}

static int qsound_load(void * context, const uint8_t * exe, size_t exe_size,
                       const uint8_t * reserved, size_t reserved_size)
{
  qsound_rom * rom = ( qsound_rom * ) context;

  for (;;)
  {
    char s[4];
    if ( exe_size < 11 ) break;
    memcpy( s, exe, 3 ); exe += 3; exe_size -= 3;
    s [3] = 0;
    uint32_t dataofs = *(uint32_t*)exe; exe += 4; exe_size -= 4;
    uint32_t datasize = *(uint32_t*)exe; exe += 4; exe_size -= 4;
    if ( datasize > exe_size )
      return -1;

    rom->upload_section( s, dataofs, exe, datasize );

    exe += datasize;
    exe_size -= datasize;
  }

  return 0;
}

static __inline__ uint32_t Endian_Swap32(uint32_t x) {
        return((x<<24)|((x<<8)&0x00FF0000)|((x>>8)&0x0000FF00)|(x>>24));
}

static bool Load(QSFContext* r)
{
  if (psf_load(r->file.c_str(), &psf_file_system, 0x41,
               0, 0, psf_info_meta, r, 0) <= 0)
  {
    delete r;
    return false;
  }

  r->qsound_state.resize(qsound_get_state_size());
  void* pEmu = &r->qsound_state[0];
  qsound_clear_state(pEmu);
  r->rom.clear();

  if (psf_load(r->file.c_str(), &psf_file_system, 0x41,
               qsound_load, &r->rom, 0, 0, 0) < 0)
  {
    delete r;
    return false;
  }

  if(r->rom.m_aKey.size() == 11)
  {
    uint8_t * ptr = &r->rom.m_aKey[0];
    uint32_t swap_key1 = Endian_Swap32( *( uint32_t * )( ptr +  0 ) );
    uint32_t swap_key2 = Endian_Swap32( *( uint32_t * )( ptr +  4 ) );
    uint32_t addr_key  = Endian_Swap32( *( uint16_t * )( ptr +  8 ) );
    uint8_t  xor_key   =                                      *( ptr + 10 );
    qsound_set_kabuki_key( pEmu, swap_key1, swap_key2, addr_key, xor_key );
  }
  else
  {
    qsound_set_kabuki_key( pEmu, 0, 0, 0, 0 );
  }

  qsound_set_z80_rom( pEmu, &r->rom.m_aZ80ROM[0], r->rom.m_aZ80ROM.size() );
  qsound_set_sample_rom( pEmu, &r->rom.m_aSampleROM[0], r->rom.m_aSampleROM.size() );
  r->pos = 0;

  return true;
}

class CQSFCodec : public kodi::addon::CInstanceAudioDecoder,
                  public kodi::addon::CAddonBase
{
public:
  CQSFCodec(KODI_HANDLE instance) :
    CInstanceAudioDecoder(instance) {}

  virtual ~CQSFCodec()
  {
  }

  virtual bool Init(const std::string& filename, unsigned int filecache,
                    int& channels, int& samplerate,
                    int& bitspersample, int64_t& totaltime,
                    int& bitrate, AEDataFormat& format,
                    std::vector<AEChannel>& channellist) override
  {
    if (qsound_init())
      return false;

    ctx.sample_buffer.Create(16384);
    ctx.file = filename;
    if (!Load(&ctx))
      return false;

    totaltime = ctx.length;
    static enum AEChannel map[3] = {
      AE_CH_FL, AE_CH_FR, AE_CH_NULL
    };
    format = AE_FMT_S16NE;
    channellist = { AE_CH_FL, AE_CH_FR };
    channels = 2;
    bitspersample = 16;
    bitrate = 0.0;
    samplerate = 44100;

    return true;
  }

  virtual int ReadPCM(uint8_t* buffer, int size, int& actualsize) override
  {
    if (ctx.pos >= ctx.length*44100*4/1000)
      return 1;

    if (ctx.sample_buffer.getMaxReadSize() == 0) {
      short temp[4096];
      unsigned written=2048;
      qsound_execute(&ctx.qsound_state[0], 0x7FFFFFFF, temp, &written);
      ctx.sample_buffer.WriteData((const char*)temp, written*4);
    }

    int tocopy = std::min(size, (int)ctx.sample_buffer.getMaxReadSize());
    ctx.sample_buffer.ReadData((char*)buffer, tocopy);
    ctx.pos += tocopy;
    actualsize = tocopy;
    return 0;
  }

  virtual int64_t Seek(int64_t time) override
  {
    int64_t pos = time*44100*4/1000;
    if (pos < ctx.pos)
    {
      Load(&ctx);
    }
    while (ctx.pos < pos)
    {
      short temp[4096];
      unsigned written=std::min((pos-ctx.pos)/4, (int64_t)2048);
      qsound_execute(&ctx.qsound_state[0], 0x7FFFFFFF, temp, &written);
      ctx.pos += written*4;
    }

    return time;
  }

  virtual bool ReadTag(const std::string& file, std::string& title,
                       std::string& artist, int& length) override
  {
    QSFContext result;
    if (psf_load(file.c_str(), &psf_file_system, 0x41, 0, 0,
                 psf_info_meta, &result, 0) <= 0)
    {
      return false;
    }
    const char* rslash = strrchr(file.c_str(),'/');
    if (!rslash)
      rslash = strrchr(file.c_str(),'\\');
    title = rslash+1;
    artist = result.album;
    length = result.length/1000;
    return true;
  }

private:
  QSFContext ctx;
};


class ATTRIBUTE_HIDDEN CMyAddon : public kodi::addon::CAddonBase
{
public:
  CMyAddon() { }
  virtual ADDON_STATUS CreateInstance(int instanceType, std::string instanceID, KODI_HANDLE instance, KODI_HANDLE& addonInstance) override
  {
    addonInstance = new CQSFCodec(instance);
    return ADDON_STATUS_OK;
  }
  virtual ~CMyAddon()
  {
  }
};


ADDONCREATOR(CMyAddon);
