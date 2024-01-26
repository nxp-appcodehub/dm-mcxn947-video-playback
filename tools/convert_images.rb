# Copyright 2023 NXP
# All rights reserved.
#
# SPDX-License-Identifier: BSD-3-Clause

require 'bundler'

Bundler::require

require 'rmagick'
include Magick

IMAGE_DIR='video'
OUTPUT_NAME='video/video_480_320.bin'

# SPI mode requires SWAPPED=1
SWAPPED = ENV['SWAPPED'].nil? ? false : true

def convert_image(filename)
    im = Image.read(filename)
    pix_ary = Array.new(480 * 320 * 2, 0)
    im[0].each_pixel do |pix, col, row|
        # Note the image format is swapped. RGB565 -> [7:0] RRRRRGGG GGGBBBBB
        low_byte_index = (row * 480 + col) * 2
        high_byte_index = low_byte_index + 1
        if SWAPPED
            pix_ary[low_byte_index] = ((pix.red / 8) << 3) | (((pix.green / 4) >> 3) & 0x07)
            pix_ary[high_byte_index] = ((pix.blue / 8) & 0x1F) | (((pix.green / 4) & 0x07) << 5)
        else
            pix_ary[high_byte_index] = ((pix.red / 8) << 3) | (((pix.green / 4) >> 3) & 0x07)
            pix_ary[low_byte_index] = ((pix.blue / 8) & 0x1F) | (((pix.green / 4) & 0x07) << 5)
        end
    end
    pix_ary
end

File.open(OUTPUT_NAME, "w+") do |f|
    Dir.glob("#{IMAGE_DIR}/*.bmp").each_with_index do |img, i|
        puts "#{i + 1} image(s) processed."
        f.write(convert_image(img).pack("C*"))
    end
end

