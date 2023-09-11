//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2023 AirenSoft. All rights reserved.
//
//==============================================================================
#include <base/ovcrypto/aes.h>

#include "bmff_private.h"
#include "cenc.h"
#include "modules/bitstream/h264/h264_parser.h"
#include "modules/bitstream/h264/h264_decoder_configuration_record.h"

namespace bmff
{
    Encryptor::Encryptor(const std::shared_ptr<const MediaTrack> &media_track, const CencProperty &cenc_property)
    {
        _cenc_property = cenc_property;
        _media_track = media_track;

        if (_cenc_property.scheme == CencProtectScheme::Cbcs)
        {
            if (_media_track->GetMediaType() == cmn::MediaType::Video)
            {
                std::function<bool(const uint8_t *, size_t, uint8_t*, bool)> cbc = std::bind(&Encryptor::EncryptCbc, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);

                _encrypt_func = std::bind(&Encryptor::EncryptPattern, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, cbc);
            }
            else if (_media_track->GetMediaType() == cmn::MediaType::Audio)
            {
                _cenc_property.crypt_bytes_block = 1;
                _cenc_property.skip_bytes_block = 0;
                _encrypt_func = std::bind(&Encryptor::EncryptCbc, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
            }
        }
    }

    bool Encryptor::Encrypt(const Sample &clear_sample, Sample &cipher_sample)
    {
        if (_cenc_property.scheme == CencProtectScheme::None || _encrypt_func == nullptr)
        {
            cipher_sample = clear_sample;
            return true;
        }

        std::vector<Sample::SubSample> sub_samples;
        if (GenerateSubSamples(clear_sample._media_packet, sub_samples) == false)
        {
            logte("Failed to generate subsamples");
            return false;
        }

        // Pattern Encryption
        auto clear_data = clear_sample._media_packet->GetData();
        auto cipher_data = std::make_shared<ov::Data>(clear_data->GetLength());

        if (EncryptInternal(clear_data, cipher_data, sub_samples) == false)
        {
            logte("Failed to encrypt");
            return false;
        }

        auto cipher_media_packet = clear_sample._media_packet->ClonePacket();
        cipher_media_packet->SetData(cipher_data);

        cipher_sample._media_packet = cipher_media_packet;
        cipher_sample._sai._sub_samples = sub_samples;

        return true;
    }

    bool Encryptor::GenerateSubSamples(const std::shared_ptr<const MediaPacket> &media_packet, std::vector<Sample::SubSample> &sub_samples)
	{
		if (media_packet == nullptr)
		{
			return false;
		}

		sub_samples.clear();

		switch (media_packet->GetBitstreamFormat())
		{
			case cmn::BitstreamFormat::H264_AVCC:
				return GenerateSubSamplesFromH264(media_packet, sub_samples);
            case cmn::BitstreamFormat::HVCC:
                // TODO: Implement
                return true;
			default:
				// Others, full sample encryption
				return true;
		}

		return false;
	}

	bool Encryptor::GenerateSubSamplesFromH264(const std::shared_ptr<const MediaPacket> &media_packet, std::vector<Sample::SubSample> &sub_samples)
	{
		auto avcc = std::static_pointer_cast<AVCDecoderConfigurationRecord>(_media_track->GetDecoderConfigurationRecord());
		auto nal_length_size = avcc->LengthMinusOne() + 1;

        uint32_t total_bytes = 0;
		uint32_t clear_bytes = 0;
		uint32_t cipher_bytes = 0;

		ov::ByteStream read_stream(media_packet->GetData());
		while (read_stream.Remained() > 0)
		{
			size_t nal_length = 0;
			switch (nal_length_size)
			{
				case 1:
					if (read_stream.IsRemained(1) == false)
					{
						logte("NAL length size is 1, but buffer length is less than 1");
						return false;
					}

					nal_length = read_stream.Read8();
					break;
				case 2:
					if (read_stream.IsRemained(2) == false)
					{
						logte("NAL length size is 2, but buffer length is less than 2");
						return false;
					}

					nal_length = read_stream.ReadBE16();
					break;
				case 4:
					if (read_stream.IsRemained(4) == false)
					{
						logte("NAL length size is 4, but buffer length is less than 4");
						return false;
					}

					nal_length = read_stream.ReadBE32();
					break;
				default:
					logte("Invalid length size (%d)", nal_length_size);
					return false;
			}

			if (read_stream.IsRemained(nal_length) == false)
			{
				logte("NAL length (%d) is greater than buffer length (%d)", nal_length, read_stream.Remained());
				return false;
			}

			auto nalu = read_stream.GetRemainData(nal_length);
			read_stream.Skip(nal_length);

			H264NalUnitHeader nal_header;
			if (H264Parser::ParseNalUnitHeader(nalu->GetDataAs<uint8_t>(), H264_NAL_UNIT_HEADER_SIZE, nal_header) == true)
			{
				if (nal_header.IsVideoSlice())
				{
					// Get Slice Header Size
					H264SliceHeader slice_header;
					if (H264Parser::ParseSliceHeader(nalu->GetDataAs<uint8_t>(), nalu->GetLength(), slice_header, avcc) == false)
					{
						logte("Failed to parse slice header");
						return false;
					}

					// Calc subsample
					// Clear bytes : Nal Length Size(1 or 2 or 4) + Nal Header Length(1) + Slice Header Size
					// Protected bytes : Nal Length - Clear bytes
					clear_bytes += nal_length_size + H264_NAL_UNIT_HEADER_SIZE + slice_header.GetHeaderSizeInBytes();
					cipher_bytes = nal_length - (H264_NAL_UNIT_HEADER_SIZE + slice_header.GetHeaderSizeInBytes());
                    total_bytes += clear_bytes + cipher_bytes;
                    
					sub_samples.emplace_back(clear_bytes, cipher_bytes);

                    clear_bytes = 0;
                    cipher_bytes = 0;

					logtd("VCL NAL Unit Type : %d, Clear Bytes : %u, Protected Bytes : %u", nal_header.GetNalUnitType(), clear_bytes, cipher_bytes);
				}
				else
				{
					// Not a video slice
                    // it will be added to the subsample of VCL NAL Unit
					clear_bytes += nal_length_size + nal_length;

					logtd("NonVCL NAL Unit Type : %d, Clear Bytes : %u", nal_header.GetNalUnitType(), clear_bytes);
				}
			}
            else
            {
                logtc("Failed to parse NAL Unit Header");
            }
		}

        if (clear_bytes > 0)
        {
            logti("Last NAL Unit is not a video slice, clear bytes : %u", clear_bytes);
            sub_samples.emplace_back(clear_bytes, cipher_bytes);
            total_bytes += clear_bytes;
        }

        if (total_bytes != media_packet->GetData()->GetLength())
        {
            logte("Total subsample bytes (%u) is not equal to sample data length (%u)", total_bytes, media_packet->GetData()->GetLength());
            return false;
        }

		logtd("Subsample count : %d Total subsamples : %u / %u", sub_samples.size(), total_bytes, media_packet->GetData()->GetLength());

		return true;
	}

	bool Encryptor::EncryptInternal(const std::shared_ptr<const ov::Data> &clear_sample_data, std::shared_ptr<ov::Data> &encrypted_sample_data, const std::vector<Sample::SubSample> &sub_samples)
	{
        if (clear_sample_data == nullptr || _encrypt_func == nullptr)
        {
            return false;
        }

        const uint8_t *clear_data_ptr = clear_sample_data->GetDataAs<uint8_t>();
        size_t clear_data_length = clear_sample_data->GetLength();
        
        encrypted_sample_data->SetLength(clear_sample_data->GetLength());
        uint8_t *encrypted_data_ptr = encrypted_sample_data->GetWritableDataAs<uint8_t>();
        
        size_t offset = 0;

        if (sub_samples.empty())
        {
            // Full sample encryption
            _encrypt_func(clear_data_ptr, clear_data_length, encrypted_data_ptr, true);
        }
        else
        {
            // Sample encryption 
            int sub_sample_index = 0;
            for (const auto &sub_sample : sub_samples)
            {
                if (sub_sample.clear_bytes > 0)
                {
                    memcpy(encrypted_data_ptr + offset, clear_data_ptr + offset, sub_sample.clear_bytes);
                    offset += sub_sample.clear_bytes;
                }

                if (sub_sample.cipher_bytes > 0)
                {
                    _encrypt_func(clear_data_ptr + offset, sub_sample.cipher_bytes, encrypted_data_ptr + offset, true);
                    offset += sub_sample.cipher_bytes;
                }

                sub_sample_index ++;
            }
        }

        return true;
	}

    // source is a sub-sample data
    bool Encryptor::EncryptPattern(const uint8_t *source, size_t source_size, uint8_t *dest, bool last_block, std::function<bool(const uint8_t*, size_t, uint8_t*, bool)>(encrypt_func))
    {
        logtd("EncryptPattern - Source Size : %u, Last Block : %s", source_size, last_block ? "true" : "false");

        // Crypt Bytes Block - Skip Bytes Block
        while (source_size > 0)
        {
            const size_t crypt_byte_size = _cenc_property.crypt_bytes_block * AES_BLOCK_SIZE;

            // If the source size is less than the crypt_byte_size, partial encryption is performed.
            if (source_size <= crypt_byte_size)
            {
                if (source_size >= AES_BLOCK_SIZE)
                {
                    const size_t clear_data_length_aligned = source_size / AES_BLOCK_SIZE * AES_BLOCK_SIZE;

                    if (encrypt_func(source, clear_data_length_aligned, dest, last_block) == false)
                    {
                        return false;
                    }

                    source += clear_data_length_aligned;
                    dest += clear_data_length_aligned;
                    source_size -= clear_data_length_aligned;
                }

                memcpy(dest, source, source_size);

                return true;
            }

            // check this is the last cipher block
            bool last = last_block && source_size <= (AES_BLOCK_SIZE - 1) + crypt_byte_size + (_cenc_property.skip_bytes_block * AES_BLOCK_SIZE);
            if (encrypt_func(source, crypt_byte_size, dest, last) == false)
            {
                return false;
            }

            source += crypt_byte_size;
            dest += crypt_byte_size;
            source_size -= crypt_byte_size;

            // Skip Bytes Block
            const size_t skip_byte_size = std::min(static_cast<size_t>(_cenc_property.skip_bytes_block * AES_BLOCK_SIZE), source_size);
            memcpy(dest, source, skip_byte_size);

            source += skip_byte_size;
            dest += skip_byte_size;
            source_size -= skip_byte_size;
        }
        
        return true;
    }

    bool Encryptor::EncryptCbc(const uint8_t *source, size_t source_size, uint8_t *dest, bool last_block)
    {
        logtd("EncryptCbc - Source Size : %u, Last Block : %s", source_size, last_block ? "true" : "false");

        if (_aes.IsInitialized() == false)
        {
            auto key = _cenc_property.key->GetDataAs<uint8_t>();
            auto key_len = _cenc_property.key->GetLength();
            auto iv = _cenc_property.iv->GetDataAs<uint8_t>();
            auto iv_len = _cenc_property.iv->GetLength();

            // No Padding
            if (_aes.Initialize(EVP_aes_128_cbc(), key, key_len, iv, iv_len, false) == false)
            {
                return false;
            }

            logtd("AES Initialized");
        }

        const size_t residual_size = source_size % AES_BLOCK_SIZE;
        const size_t cbc_size = source_size - residual_size;

        _aes.Update(source, cbc_size, dest);

        if (residual_size > 0)
        {
            // unencrypted data, no padding mode
            memcpy(dest + cbc_size, source + cbc_size, residual_size);
        }

        if (last_block == true)
        {
            _aes.Finalize(dest);
        }
        
        return true;   
    }
}