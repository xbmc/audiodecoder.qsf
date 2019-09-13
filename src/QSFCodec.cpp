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

#include "CircularBuffer.h"
#include "QSoundRom.h"

#include <kodi/addon-instance/AudioDecoder.h>
#include <kodi/Filesystem.h>
#include <math.h>

extern "C"
{
#include "qsound.h"
#include "psflib.h"

static void* psf_file_fopen(const char * uri)
{
  if (!uri)
    return nullptr;

  std::string usedFile = uri;
  kodi::vfs::CFile* file = new kodi::vfs::CFile;
  if (!file->OpenFile(usedFile, 0))
  {
    // Transform to lower case and try on OpenFile again.
    // Windows does not look about case thats why some not works.
    std::string fileName = kodi::vfs::GetFileName(usedFile);
    std::transform(fileName.begin(), fileName.end(), fileName.begin(), ::tolower);
    usedFile = kodi::vfs::GetDirectoryName(usedFile) + fileName;
    if (!file->OpenFile(usedFile, 0))
    {
      delete file;
      return nullptr;
    }
  }

  return file;
}

static size_t psf_file_fread(void * buffer, size_t size, size_t count, void * handle)
{
  kodi::vfs::CFile* file = static_cast<kodi::vfs::CFile*>(handle);
  return file->Read(buffer, size*count);
}

static int psf_file_fseek(void * handle, int64_t offset, int whence)
{
  kodi::vfs::CFile* file = static_cast<kodi::vfs::CFile*>(handle);
  return file->Seek(offset, whence) > -1 ? 0 : -1;
}

static int psf_file_fclose(void * handle)
{
  delete static_cast<kodi::vfs::CFile*>(handle);

  return 0;
}

static long psf_file_ftell(void * handle)
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

static void print_message(void * context, const char * message)
{
  kodi::Log(ADDON_LOG_DEBUG, "QSF codec message: '%s'", message);
}

#define BORK_TIME 0xC0CAC01A
static unsigned long parse_time_crap(const char *input)
{
  unsigned long value = 0;
  unsigned long multiplier = 1000;
  const char * ptr = input;
  unsigned long colon_count = 0;

  while (*ptr && ((*ptr >= '0' && *ptr <= '9') || *ptr == ':'))
  {
    colon_count += *ptr == ':';
    ++ptr;
  }
  if (colon_count > 2)
    return BORK_TIME;
  if (*ptr && *ptr != '.' && *ptr != ',')
    return BORK_TIME;
  if (*ptr)
    ++ptr;
  while (*ptr && *ptr >= '0' && *ptr <= '9')
    ++ptr;
  if (*ptr)
    return BORK_TIME;

  ptr = strrchr(input, ':');
  if (!ptr)
    ptr = input;
  for (;;)
  {
    char * end;
    if (ptr != input)
      ++ptr;
    if (multiplier == 1000)
    {
      double temp = strtod(ptr, nullptr);
      if (temp >= 60.0)
        return BORK_TIME;
      value = (long)(temp * 1000.0f);
    }
    else
    {
      unsigned long temp = strtoul(ptr, &end, 10);
      if (temp >= 60 && multiplier < 3600000)
        return BORK_TIME;
      value += temp * multiplier;
    }
    if (ptr == input)
      break;
    ptr -= 2;
    while (ptr > input && *ptr != ':')
      --ptr;
    multiplier *= 60;
  }

  return value;
}

struct QSFContext
{
  int length = 0;
  int fade = 0;
  int year;
  std::string title;
  std::string artist;
  std::string album;
};

static int psf_info_meta(void * context, const char * name, const char * value)
{
  QSFContext* qsf = (QSFContext*)context;

  if (!strcasecmp(name, "title"))
    qsf->title = value;
  else if (!strcasecmp(name, "game"))
    qsf->album = value;
  else if (!strcasecmp(name, "artist"))
    qsf->artist = value;
  else if (!strcasecmp(name, "year"))
    qsf->year = atoi(value);
  else if (!strcasecmp(name, "length"))
  {
    int temp = parse_time_crap(value);
    if (temp != BORK_TIME)
      qsf->length = temp;
  }
  else if (!strcasecmp(name, "fade"))
  {
    int temp = parse_time_crap(value);
    if (temp != BORK_TIME)
      qsf->fade = temp;
  }

  return 0;
}

static int qsound_load(void * context, const uint8_t * exe, size_t exe_size,
                       const uint8_t * reserved, size_t reserved_size)
{
  qsound_rom* rom = (qsound_rom*)context;

  for (;;)
  {
    char s[4];
    if (exe_size < 11)
      break;
    memcpy(s, exe, 3); exe += 3; exe_size -= 3;
    s [3] = 0;
    uint32_t dataofs = *(uint32_t*)exe; exe += 4; exe_size -= 4;
    uint32_t datasize = *(uint32_t*)exe; exe += 4; exe_size -= 4;
    if (datasize > exe_size)
      return -1;

    rom->upload_section(s, dataofs, exe, datasize);

    exe += datasize;
    exe_size -= datasize;
  }

  return 0;
}

} /* extern "C"  */

static __inline__ uint32_t Endian_Swap32(uint32_t x) {
        return((x<<24)|((x<<8)&0x00FF0000)|((x>>8)&0x0000FF00)|(x>>24));
}

class ATTRIBUTE_HIDDEN CQSFCodec : public kodi::addon::CInstanceAudioDecoder
{
public:
  CQSFCodec(KODI_HANDLE instance) :
    CInstanceAudioDecoder(instance) {}

  virtual ~CQSFCodec() = default;

  bool Init(const std::string& filename, unsigned int filecache,
            int& channels, int& samplerate,
            int& bitspersample, int64_t& totaltime,
            int& bitrate, AEDataFormat& format,
            std::vector<AEChannel>& channellist) override;
  int ReadPCM(uint8_t* buffer, int size, int& actualsize) override;
  int64_t Seek(int64_t time) override;
  bool ReadTag(const std::string& file, std::string& title,
               std::string& artist, int& length) override;

private:
  bool Load();

  inline uint64_t time_to_samples(double p_time, uint32_t p_sample_rate)
  {
    return (uint64_t)floor((double)p_sample_rate * p_time + 0.5);
  }

  inline void calcfade()
  {
    m_songLength = mul_div(m_tagSong_ms-m_posDelta,24038,1000);
    m_fadeLength = mul_div(m_tagFade_ms,24038,1000);
  }

  inline int mul_div(int number, int numerator, int denominator)
  {
    long long ret = number;
    ret *= numerator;
    ret /= denominator;
    return (int) ret;
  }

  int m_cfgEndSilenceSeconds = 5;
  bool m_cfgSuppressOpeningSilence = true;
  bool m_cfgSuppressEndSilence = true;
  int m_cfgDefaultLength = 170000;
  int m_cfgDefaultFade = 10000;

  bool m_noLoop = true;
  bool m_eof = false;

  std::string m_usedFilename;
  std::vector<uint8_t> m_qsoundState;
  std::vector<int16_t> m_sampleBuffer;
  circular_buffer<int16_t> m_silenceTestBuffer = 0;
  qsound_rom m_rom;
  int m_err = 0;
  int64_t m_qsfEmuPos = 0;
  int m_dataWritten = 0;
  int m_remainder = 0;
  int m_posDelta = 0;
  int m_startsilence = 0;
  int m_silence = 0;

  int m_songLength;
  int m_fadeLength;
  int m_tagSong_ms;
  int m_tagFade_ms;

  QSFContext m_ctx;
};

bool CQSFCodec::Init(const std::string& filename, unsigned int filecache,
                     int& channels, int& samplerate,
                     int& bitspersample, int64_t& totaltime,
                     int& bitrate, AEDataFormat& format,
                     std::vector<AEChannel>& channellist)
{
  if (qsound_init())
  {
    kodi::Log(ADDON_LOG_ERROR, "QSound emulator static initialization failed");
    return false;
  }

  m_usedFilename = filename;
  if (!Load())
    return false;

  totaltime = m_ctx.length;
  format = AE_FMT_S16NE;
  channellist = { AE_CH_FL, AE_CH_FR };
  channels = 2;
  bitspersample = 16;
  bitrate = 0;
  samplerate = 24038;

  return true;
}

int CQSFCodec::ReadPCM(uint8_t* buffer, int size, int& actualsize)
{
  if (m_err < 0)
    return 1;
  if (m_eof && !m_silenceTestBuffer.data_available())
    return -1;
  if (m_noLoop && m_tagSong_ms && (m_posDelta + mul_div(m_dataWritten, 1000, 24038)) >= m_tagSong_ms + m_tagFade_ms)
    return 1;

  unsigned int written = 0;

  int usedSize = size / 2 / sizeof(int16_t);

  int samples;

  if (m_noLoop)
  {
    samples = (m_songLength + m_fadeLength) - m_dataWritten;
    if (samples > usedSize)
      samples = usedSize;
  }
  else
  {
    samples = usedSize;
  }

  if (m_cfgSuppressEndSilence)
  {
    m_sampleBuffer.resize(usedSize * 2);

    if (!m_eof)
    {
      unsigned free_space = m_silenceTestBuffer.free_space() / 2;
      while (free_space)
      {
        unsigned samples_to_render;
        if (m_remainder)
        {
          samples_to_render = m_remainder;
          m_remainder = 0;
        }
        else
        {
          samples_to_render = free_space;
          if (samples_to_render > usedSize)
            samples_to_render = usedSize;
          m_err = qsound_execute(m_qsoundState.data(), 0x7FFFFFFF, m_sampleBuffer.data(), & samples_to_render);
          if (m_err < 0)
          {
            kodi::Log(ADDON_LOG_ERROR, "Execution halted with an error: '%i'", m_err);
            return 1;
          }
          if (!samples_to_render)
          {
            kodi::Log(ADDON_LOG_ERROR, "Execution no samples to render");
            return 1;
          }
        }
        m_silenceTestBuffer.write(m_sampleBuffer.data(), samples_to_render * 2);
        free_space -= samples_to_render;
      }
    }

    if (m_silenceTestBuffer.test_silence())
    {
      m_eof = true;
      return -1;
    }

    written = m_silenceTestBuffer.data_available() / 2;
    if (written > samples)
      written = samples;
    m_silenceTestBuffer.read(m_sampleBuffer.data(), written * 2);
  }
  else
  {
    m_sampleBuffer.resize(samples * 2);

    if (m_remainder)
    {
      written = m_remainder;
      m_remainder = 0;
    }
    else
    {
      written = samples;

      m_err = qsound_execute(m_qsoundState.data(), 0x7FFFFFFF, m_sampleBuffer.data(), & written);
      if (m_err < 0)
      {
        kodi::Log(ADDON_LOG_ERROR, "Execution halted with an error: '%i'", m_err);
        return 1;
      }
      if (!written)
      {
        kodi::Log(ADDON_LOG_ERROR, "Execution no written data");
        return 1;
      }
    }
  }

  m_qsfEmuPos += double(written) / 24038.;

  int d_start, d_end;
  d_start = m_dataWritten;
  m_dataWritten += written;
  d_end = m_dataWritten;

  if (m_tagSong_ms && d_end > m_songLength && m_noLoop)
  {
    short * foo = m_sampleBuffer.data();
    for (int n = d_start; n < d_end; ++n)
    {
      if (n > m_songLength)
      {
        if (n > m_songLength + m_fadeLength)
        {
          *(uint32_t*)foo = 0;
        }
        else
        {
          int bleh = m_songLength + m_fadeLength - n;
          foo[0] = mul_div(foo[0], bleh, m_fadeLength);
          foo[1] = mul_div(foo[1], bleh, m_fadeLength);
        }
      }
      foo += 2;
    }
  }

  memcpy(buffer, m_sampleBuffer.data(), written*2*sizeof(int16_t));
  actualsize = written*2*sizeof(int16_t);

  return 0;
}

int64_t CQSFCodec::Seek(int64_t time)
{
  double p_seconds = double(time) / 1000.0;
  m_eof = false;

  double buffered_time = (double)(m_silenceTestBuffer.data_available() / 2) / 24038.0;

  m_qsfEmuPos += buffered_time;

  m_silenceTestBuffer.reset();

  if (p_seconds < m_qsfEmuPos)
  {
    Load();
  }
  unsigned int howmany = (int)(time_to_samples(p_seconds - m_qsfEmuPos, 24038));

  // more abortable, and emu doesn't like doing huge numbers of samples per call anyway
  void* pEmu = m_qsoundState.data();
  while (howmany)
  {
    unsigned todo = howmany;
    if (todo > 2048)
      todo = 2048;
    int rtn = qsound_execute(pEmu, 0x7FFFFFFF, 0, &todo);
    if (rtn < 0 || ! todo)
    {
      m_eof = true;
      return -1;
    }
    howmany -= todo;
  }

  m_dataWritten = 0;
  m_posDelta = (int)(p_seconds * 1000.);
  m_qsfEmuPos = p_seconds;

  calcfade();

  return time;
}

bool CQSFCodec::Load()
{
  m_qsoundState.resize(qsound_get_state_size());
  void* pEmu = m_qsoundState.data();
  qsound_clear_state(pEmu);
  m_rom.clear();

  if (psf_load(m_usedFilename.c_str(), &psf_file_system, 0x41,
               qsound_load, &m_rom, psf_info_meta, &m_ctx, 0, print_message, nullptr) < 0)
  {
    kodi::Log(ADDON_LOG_ERROR, "Failed to load '%s'", m_usedFilename.c_str());
    return false;
  }

  m_tagSong_ms = m_ctx.length;
  if (!m_tagSong_ms)
    m_tagSong_ms = m_cfgDefaultLength;

  m_tagFade_ms = m_ctx.fade;
  if (!m_tagFade_ms)
    m_tagFade_ms = m_cfgDefaultFade;

  if (m_rom.m_aKey.size() == 11)
  {
    uint8_t * ptr = &m_rom.m_aKey[0];
    uint32_t swap_key1 = Endian_Swap32(*(uint32_t*)(ptr +  0));
    uint32_t swap_key2 = Endian_Swap32(*(uint32_t*)(ptr +  4));
    uint32_t addr_key  = Endian_Swap32(*(uint16_t*)(ptr +  8));
    uint8_t  xor_key   =                          *(ptr + 10);
    qsound_set_kabuki_key(pEmu, swap_key1, swap_key2, addr_key, xor_key);
  }
  else
  {
    qsound_set_kabuki_key(pEmu, 0, 0, 0, 0);
  }

  qsound_set_z80_rom(pEmu, &m_rom.m_aZ80ROM[0], m_rom.m_aZ80ROM.size());
  qsound_set_sample_rom(pEmu, &m_rom.m_aSampleROM[0], m_rom.m_aSampleROM.size());

  m_eof = false;
  m_err = 0;
  m_qsfEmuPos = 0;
  m_startsilence = 0;
  m_silence = 0;

  calcfade();

  unsigned int skip_max = m_cfgEndSilenceSeconds * 24038;

  if (m_cfgSuppressOpeningSilence) // ohcrap
  {
    for (;;)
    {
      unsigned int skip_howmany = skip_max - m_silence;
      if (skip_howmany > 1024)
        skip_howmany = 1024;
      m_sampleBuffer.resize(skip_howmany * 2);
      int rtn = qsound_execute(pEmu, 0x7FFFFFFF, m_sampleBuffer.data(), &skip_howmany);
      if (rtn < 0)
        return false;
      short * foo = m_sampleBuffer.data();
      unsigned int i;
      for (i = 0; i < skip_howmany; ++i)
      {
        if (foo[0] || foo[1])
          break;
        foo += 2;
      }
      m_silence += i;
      if (i < skip_howmany)
      {
        m_remainder = skip_howmany - i;
        memmove(m_sampleBuffer.data(), foo, m_remainder * sizeof(short) * 2);
        break;
      }
      if (m_silence >= skip_max)
      {
        m_eof = true;
        break;
      }
    }

    m_startsilence += m_silence;
    m_silence = 0;
  }

  if (m_cfgSuppressEndSilence)
    m_silenceTestBuffer.resize(skip_max * 2);

  return true;
}

bool CQSFCodec::ReadTag(const std::string& file, std::string& title,
                        std::string& artist, int& length)
{
  QSFContext result;
  if (psf_load(file.c_str(), &psf_file_system, 0x41, nullptr, nullptr,
               psf_info_meta, &result, 0, print_message, nullptr) <= 0)
  {
    return false;
  }

  if (!result.title.empty())
  {
    title = result.title;
  }
  else
  {
    title = kodi::vfs::GetFileName(file);
    title.erase(title.find_last_of("."), std::string::npos);
  }

  if (!result.album.empty())
    artist = result.album;
  else if (!result.artist.empty())
    artist = result.artist;
  length = result.length / 1000;
  return true;
}


class ATTRIBUTE_HIDDEN CMyAddon : public kodi::addon::CAddonBase
{
public:
  CMyAddon() = default;
  ADDON_STATUS CreateInstance(int instanceType, std::string instanceID, KODI_HANDLE instance, KODI_HANDLE& addonInstance) override
  {
    addonInstance = new CQSFCodec(instance);
    return ADDON_STATUS_OK;
  }
  virtual ~CMyAddon() = default;
};


ADDONCREATOR(CMyAddon);
