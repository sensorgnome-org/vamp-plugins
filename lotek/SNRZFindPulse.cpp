/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
  Vamp

  An API for audio analysis and feature extraction plugins.

  Centre for Digital Music, Queen Mary, University of London.
  Copyright 2006 Chris Cannam.

  VAMP license:

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation
  files (the "Software"), to deal in the Software without
  restriction, including without limitation the rights to use, copy,
  modify, merge, publish, distribute, sublicense, and/or sell copies
  of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
    
  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.
    
  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
  ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
  CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
    
  Except as contained in this notice, the names of the Centre for
  Digital Music; Queen Mary, University of London; and Chris Cannam
  shall not be used in advertising or otherwise to promote the sale,
  use or other dealings in this Software without prior written
  authorization.
    
  SNRZFindPulse.cpp - probabilistic time domain pulse finder

  Copyright 2014 John Brzustowski

  License: GPL v 2.0 or later.  This is required in order to use fftw.

*/

#include "SNRZFindPulse.h"

using std::stringstream;
using std::string;
using std::vector;
using std::cerr;
using std::endl;

SNRZFindPulse::SNRZFindPulse(float inputSampleRate) :
    Plugin(inputSampleRate),
    m_stepSize(0),
    m_blockSize(0),
    m_plen(m_default_plen),
    m_bkgd(m_default_bkgd),
    m_fft_win(m_default_fft_win),
    m_fft_pad(m_default_fft_pad),
    m_fft_overlap(m_default_fft_overlap),
    m_min_Z(m_default_min_Z),
    m_min_SNR_dB(m_default_min_SNR_dB),
    m_max_noise_for_Z (undB(m_default_max_noise_for_Z_dB)),
    m_min_freq (m_default_min_freq),
    m_max_freq (m_default_max_freq),
    m_batch_host (false),
    m_spf(0),
    m_fest(0)
    //    m_dcma (10000)
{
    // silently fail if wisdom cannot be found
    FILE *f = fopen(fftw_wisdom_filename, "r");
    if (f) {
        (void) fftwf_import_wisdom_from_file(f);
        fclose(f);
    }
}

SNRZFindPulse::~SNRZFindPulse()
{
    if (m_spf)
        delete m_spf;
    if (m_fest)
        delete m_fest;

    // silently fail if we can't export wisdom
    FILE *f = fopen(fftw_wisdom_filename, "wb");
    if (f) {
        (void) fftwf_export_wisdom_to_file(f);
        fclose(f);
    }
}

string
SNRZFindPulse::getIdentifier() const
{
    // note: to allow use by legacy deployment files,
    // we give this the same name as was used by the very
    // different previous incarnation into early 2014

    return "findpulsefdbatch";
}

string
SNRZFindPulse::getName() const
{
    return "Spectral Pulse Finder (SNR or Z)";
}

string
SNRZFindPulse::getDescription() const
{
    return "Find pulses in the spectrum with minimum SNR or Z-score";
}

string
SNRZFindPulse::getMaker() const
{
    return "sensorgnome.org  jbrzusto@fastmail.fm";
}

int
SNRZFindPulse::getPluginVersion() const
{
    return 1;
}

string
SNRZFindPulse::getCopyright() const
{
    return "GPL version 2 or later";
}

bool
SNRZFindPulse::initialise(size_t channels, size_t stepSize, size_t blockSize)
{
    if (channels < getMinChannelCount() ||
	channels > getMaxChannelCount()) return false;

    m_channels = channels;
    m_stepSize = stepSize;
    m_blockSize = blockSize;

    m_plen_samples = (m_plen / 1000.0) * m_inputSampleRate;
    m_bkgd_samples = (m_bkgd / 1000.0) * m_inputSampleRate;
    m_num_bins = m_fft_win * m_fft_pad;
    m_power_scale_dB = - 20 * log10(m_num_bins);

    // cap frequency limits at Nyquist
    if (m_min_freq > m_inputSampleRate / 2000)
        m_min_freq = m_inputSampleRate / 2000;
    if (m_max_freq > m_inputSampleRate / 2000)
        m_max_freq = m_inputSampleRate / 2000;
    
    m_bin_step = m_inputSampleRate / (1000.0 * m_num_bins);
    m_min_bin = floor(m_min_freq / m_bin_step) ;
    m_max_bin = ceil(m_max_freq / m_bin_step) ;
    
    m_num_finders = m_max_bin - m_min_bin + 1;

    m_spf = new SpectralPulseFinder (m_plen_samples, m_bkgd_samples, m_fft_win, m_fft_pad, m_fft_overlap, m_min_bin, m_max_bin, m_min_SNR_dB, m_min_Z, m_max_noise_for_Z);

    m_fest = new FreqEstimator (m_plen_samples);

    //    m_sample_buf = boost::circular_buffer < std::complex < float > > (m_plen_samples + 2 * m_bkgd_samples);
    m_sample_buf = boost::circular_buffer < std::complex < float > > (m_spf->location());

    return true;
}

void
SNRZFindPulse::reset()
{
}

SNRZFindPulse::ParameterList
SNRZFindPulse::getParameterDescriptors() const
{
    ParameterList list;

    ParameterDescriptor d;
    d.identifier = "plen";
    d.name = "Pulse Length (unit: milliseconds)";
    d.description = "Duration of a transmitted pulse in milliseconds";
    d.unit = "milliseconds";
    d.minValue = 0.1;
    d.maxValue = 500;
    d.defaultValue = SNRZFindPulse::m_default_plen;
    d.isQuantized = false;
    list.push_back(d);

    d.identifier = "bkgd";
    d.name = "Background Window Length (single-sided; unit: milliseconds)";
    d.description = "Duration of the background window on each side of the pulse";
    d.unit = "milliseconds";
    d.minValue = 0.1;
    d.maxValue = 500;
    d.defaultValue = SNRZFindPulse::m_default_bkgd;
    d.isQuantized = false;
    list.push_back(d);

    d.identifier = "fftwin";
    d.name = "FFT Win Size";
    d.description = "FFT Window Size (number of samples)";
    d.unit = "(none)";
    d.minValue = 1;
    d.maxValue = 2000;
    d.defaultValue = SNRZFindPulse::m_default_fft_win;
    d.isQuantized = false;
    list.push_back(d);

    d.identifier = "fftpad";
    d.name = "FFT Padding Factor";
    d.description = "How many times bigger is the zero-padded window size to the pure-data window";
    d.unit = "(none)";
    d.minValue = 1;
    d.maxValue = 30;
    d.defaultValue = SNRZFindPulse::m_default_fft_pad;
    d.isQuantized = true;
    d.quantizeStep = 1;
    list.push_back(d);

    d.identifier = "fftoverlap";
    d.name = "FFT Overlap (number of non-padding samples)";
    d.description = "Number of samples of overlap between consecutive FFTs (not including padding)";
    d.unit = "(none)";
    d.minValue = 1;
    d.maxValue = 1999;
    d.defaultValue = SNRZFindPulse::m_default_fft_overlap;
    d.isQuantized = true;
    d.quantizeStep = 1;
    list.push_back(d);

    d.identifier = "minsnr";
    d.name = "Minimum Signal to Noise Ratio";
    d.description = "Minimum ratio of signal (with bkgd subtracted) to bkgd, in dB";
    d.unit = "dB";
    d.minValue = 0;
    d.maxValue = 100;
    d.defaultValue = SNRZFindPulse::m_default_min_SNR_dB;
    d.isQuantized = false;
    list.push_back(d);

    d.identifier = "maxnoisez";
    d.name = "Maximum Noise For Using Z-Score (unit: dB)";
    d.description = "Noise Level (dB) below which Z score can be used instead of SNR";
    d.unit = "(none)";
    d.minValue = -100;
    d.maxValue = -30;
    d.defaultValue = SNRZFindPulse::m_default_max_noise_for_Z_dB;
    d.isQuantized = false;
    list.push_back(d);

    d.identifier = "minz";
    d.name = "Minimum Z Score";
    d.description = "Minimum Z score of pulse vs background; only in effect at low noise";
    d.unit = "(none)";
    d.minValue = 0;
    d.maxValue = 1000;
    d.defaultValue = SNRZFindPulse::m_default_min_Z;
    d.isQuantized = false;
    list.push_back(d);

    d.identifier = "minfreq";
    d.name = "Minimum Tag Offset Frequency (unit: kHz)";
    d.description = "Minimum frequency by which tag differs from receiver, in kHz";
    d.unit = "kHz";
    d.minValue = - m_inputSampleRate / 2000;
    d.maxValue = m_inputSampleRate / 2000;
    d.defaultValue = SNRZFindPulse::m_default_min_freq;
    d.isQuantized = false;
    list.push_back(d);

    d.identifier = "maxfreq";
    d.name = "Maximum Tag Offset Frequency (unit: kHz)";
    d.description = "Maximum frequency by which tag differs from receiver, in kHz";
    d.unit = "kHz";
    d.minValue = - m_inputSampleRate / 2000;
    d.maxValue = m_inputSampleRate / 2000;
    d.defaultValue = SNRZFindPulse::m_default_max_freq;
    d.isQuantized = false;
    list.push_back(d);

    d.identifier = "__batch_host__";
    d.name = "IGNORE: set automatically by audacity or vamp-alsa-host";
    d.description = "set to 1 when host needs batch output";
    d.unit = "";
    d.minValue = 0;
    d.maxValue = 1;
    d.defaultValue = SNRZFindPulse::m_batch_host;
    d.isQuantized = true;
    d.quantizeStep = 1.0;
    list.push_back(d);

    return list;
}

float
SNRZFindPulse::getParameter(string id) const
{
    if (id == "plen") {
        return m_plen;
    } else if (id == "bkgd") {
        return m_bkgd;
    } else if (id == "fftwin") {
        return m_fft_win;
    } else if (id == "fftpad") {
        return m_fft_pad;
    } else if (id == "fftoverlap") {
        return m_fft_overlap;
    } else if (id == "minsnr") {
        return m_min_SNR_dB;
    } else if (id == "maxnoisez") {
        return dB(m_max_noise_for_Z);
    } else if (id == "minz") {
        return m_min_Z;
    } else if (id == "minfreq") {
        return m_min_freq;
    } else if (id == "maxfreq") {
        return m_max_freq;
    } else if (id == "__batch_host__") {
        return m_batch_host;
    } else {
        throw std::runtime_error("invalid parameter name");
    }
}

void
SNRZFindPulse::setParameter(string id, float value)
{
    if (id == "plen") {
        SNRZFindPulse::m_default_plen = m_plen = value;
    } else if (id == "bkgd") {
        SNRZFindPulse::m_default_bkgd = m_bkgd = value;
    } else if (id == "fftwin") {
        SNRZFindPulse::m_default_fft_win = m_fft_win = value;
    } else if (id == "fftpad") {
        SNRZFindPulse::m_default_fft_pad = m_fft_pad = value;
    } else if (id == "fftoverlap") {
        SNRZFindPulse::m_default_fft_overlap = m_fft_overlap = value;
    } else if (id == "minsnr") {
        SNRZFindPulse::m_default_min_SNR_dB = m_min_SNR_dB = value;
    } else if (id == "maxnoisez") {
        SNRZFindPulse::m_default_max_noise_for_Z_dB = value;
        m_max_noise_for_Z = undB(value);
    } else if (id == "minz") {
        SNRZFindPulse::m_default_min_Z = m_min_Z = value;
    } else if (id == "minfreq") {
        SNRZFindPulse::m_default_min_freq = m_min_freq = value;
    } else if (id == "maxfreq") {
        SNRZFindPulse::m_default_max_freq = m_max_freq = value;
    } else if (id == "maxnoiseforz") {
        SNRZFindPulse::m_default_max_noise_for_Z_dB = m_max_noise_for_Z = value;
    } else if (id == "__batch_host__") {
        // kludge: parameter that affects whether this plugin
        // produces output for a batch-style host (e.g. vamp-alsa-host)
        // or for display in a GUI-style host (e.g. audacity)
        // The default value for m_batch_host is false, so it will stay
        // thus unless a host is aware of this parameter and sets it to
        // true.
        SNRZFindPulse::m_batch_host = value;
    } else {
        throw std::runtime_error("invalid parameter name");
    }
}


SNRZFindPulse::OutputList
SNRZFindPulse::getOutputDescriptors() const
{
    OutputList list;

    OutputDescriptor zc;

    zc.identifier = "pulses";
    zc.name = "Pulses";
    zc.description = "The locations and features of pulses";
    zc.unit = "";
    zc.hasFixedBinCount = true;
    zc.binCount = m_batch_host ? 3 : 0;
    zc.sampleType = OutputDescriptor::VariableSampleRate;
    zc.sampleRate = m_inputSampleRate;
    list.push_back(zc);

    return list;
}

SNRZFindPulse::FeatureSet
SNRZFindPulse::process(const float *const *inputBuffers,
                     Vamp::RealTime timestamp)
{
    FeatureSet returnFeatures;

    if (m_stepSize == 0) {
	cerr << "ERROR: SNRZFindPulse::process: "
	     << "SNRZFindPulse has not been initialised"
	     << endl;
	return returnFeatures;
    }

    for (unsigned i=0; i < m_blockSize; ++i) {
        // get sample as complex I/Q 

        std::complex < float > sample (inputBuffers[0][i], inputBuffers[1][i]);
        
        // buffer it

        m_sample_buf.push_back(sample);

        // send it to the pulse finder

        if ((*m_spf) (sample)) {
            // found pulse(s)

            // how many samples back was the centre of the pulse?
            int centre = (int) i - (int) m_spf->location() + m_plen_samples / 2.0;
            Vamp::RealTime ts = timestamp + Vamp::RealTime::frame2RealTime(centre, (size_t) m_inputSampleRate);
 
            for (auto biniter = m_spf->beginbin(); biniter != m_spf->endbin(); ++biniter) {
                int i = *biniter;
                       
                // dump the feature
                Feature feature;
                feature.hasTimestamp = true;
                feature.hasDuration = false;
            
                // The pulse timestamp is taken to be the centre of the pulse window

                feature.timestamp = ts;
     
                float sig = dB (m_spf->signal(i) - m_spf->noise(i)) + m_power_scale_dB;
                float noise = dB (m_spf->noise(i)) + m_power_scale_dB;

                // get a better estimate of frequency offset
                auto a1 = m_sample_buf.array_one();
                auto a2 = m_sample_buf.array_two();
                
                int n1 = std::min (m_plen_samples, (int) a1.second);
                int n2 = std::min (m_plen_samples - n1, 0);
                
                float freq = m_fest->get(a1.first, n1, a2.first, n2);
                
                if (fabs(freq - (i + m_min_bin) * m_plen_samples / (float) m_num_bins) > 2) {
                    continue; // don't use this pulse - the main energy is in another bin
                }

                freq *= m_inputSampleRate / (1000.0 * m_plen_samples);

                if (freq < m_min_freq || freq > m_max_freq)
                    continue;
               
                if (m_batch_host) {
                    feature.values.push_back(freq);
                    feature.values.push_back(sig);
                    feature.values.push_back(noise);
                } else {
                    std::stringstream ss;
                    ss.precision(5);
                    
                    ss << "freq: " << freq << " (kHz)"
                       << "; pwr: " << sig << " dB"
                       << "; bgkd: " << noise  << " dB"
                       << ";  bin: " << i << "; Z: " << m_spf->Z(i);
            
                    feature.label = ss.str();
                }
                returnFeatures[0].push_back(feature);
            }
        }
    }
    return returnFeatures;
}

SNRZFindPulse::FeatureSet
SNRZFindPulse::getRemainingFeatures()
{
    return FeatureSet();
}

float SNRZFindPulse::m_default_plen = 2.5; // milliseconds
float SNRZFindPulse::m_default_bkgd = 12.5; // milliseconds
int SNRZFindPulse::m_default_fft_win = 48; // 0.5 milliseconds @ 192kHz
int SNRZFindPulse::m_default_fft_pad = 1; // 
int SNRZFindPulse::m_default_fft_overlap = 24; 

double SNRZFindPulse::m_default_min_SNR_dB = 6; // minimum SNR 
double SNRZFindPulse::m_default_min_Z = 5; // z-score
double SNRZFindPulse::m_default_max_noise_for_Z_dB = -50; // z-score
float SNRZFindPulse::m_default_min_freq = -5.0; // -4 kHz
float SNRZFindPulse::m_default_max_freq =  5.0; // +4 kHz

const char * SNRZFindPulse::fftw_wisdom_filename = "./fftw_wisdom.dat";
