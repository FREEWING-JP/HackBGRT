#include <efi.h>
#include <efilib.h>

#include "types.h"
#include "config.h"
#include "util.h"

/**
 * The Print function signature.
 */
typedef UINTN print_t(IN CONST CHAR16 *fmt, ...);

/**
 * The function for debug printing; either Print or NullPrint.
 */
print_t* Debug = NullPrint;

/**
 * The configuration.
 */
static struct HackBGRT_config config = {
	.action = HackBGRT_KEEP
};

/**
 * Get the GOP (Graphics Output Protocol) pointer.
 */
static EFI_GRAPHICS_OUTPUT_PROTOCOL* GOP(void) {
	static EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
	if (!gop) {
		EFI_GUID GraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
		LibLocateProtocol(&GraphicsOutputProtocolGuid, (VOID **)&gop);
	}
	return gop;
}

/**
 * Set screen resolution. If there is no exact match, try to find a bigger one.
 *
 * @param w Horizontal resolution. 0 for max, -1 for current.
 * @param h Vertical resolution. 0 for max, -1 for current.
 */
static void SetResolution(int w, int h) {
	EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = GOP();
	if (!gop) {
		Debug(L"GOP not found!\n");
		return;
	}
	UINTN best_i = gop->Mode->Mode;
	int best_w = gop->Mode->Info->HorizontalResolution;
	int best_h = gop->Mode->Info->VerticalResolution;
	w = (w <= 0 ? w < 0 ? best_w : 0x7fffffff : w);
	h = (h <= 0 ? h < 0 ? best_h : 0x7fffffff : h);

	Debug(L"Looking for resolution %dx%d...\n", w, h);
	for (UINT32 i = gop->Mode->MaxMode; i--;) {
		int new_w = 0, new_h = 0;

		EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info = 0;
		UINTN info_size;
		if (EFI_ERROR(gop->QueryMode(gop, i, &info_size, &info))) {
			continue;
		}
		if (info_size < sizeof(*info)) {
			FreePool(info);
			continue;
		}
		new_w = info->HorizontalResolution;
		new_h = info->VerticalResolution;
		FreePool(info);

		// Sum of missing w/h should be minimal.
		int new_missing = max(w - new_w, 0) + max(h - new_h, 0);
		int best_missing = max(w - best_w, 0) + max(h - best_h, 0);
		if (new_missing > best_missing) {
			continue;
		}
		// Sum of extra w/h should be minimal.
		int new_over = max(-w + new_w, 0) + max(-h + new_h, 0);
		int best_over = max(-w + best_w, 0) + max(-h + best_h, 0);
		if (new_missing == best_missing && new_over >= best_over) {
			continue;
		}
		best_w = new_w;
		best_h = new_h;
		best_i = i;
	}
	Debug(L"Found resolution %dx%d.\n", best_w, best_h);
	if (best_i != gop->Mode->Mode) {
		gop->SetMode(gop, best_i);
	}
}

/**
 * Select the correct coordinate (manual, automatic, native)
 *
 * @param value The configured coordinate value; has special values for automatic and native.
 * @param automatic The automatically calculated alternative.
 * @param native The original coordinate.
 * @see enum HackBGRT_coordinate
 */
static int SelectCoordinate(int value, int automatic, int native) {
	if (value == HackBGRT_coord_auto) {
		return automatic;
	}
	if (value == HackBGRT_coord_native) {
		return native;
	}
	return value;
}

/**
 * Create a new XSDT with the given number of entries.
 *
 * @param xsdt0 The old XSDT.
 * @param entries The number of SDT entries.
 * @return Pointer to a new XSDT.
 */
ACPI_SDT_HEADER* CreateXsdt(ACPI_SDT_HEADER* xsdt0, UINTN entries) {
	ACPI_SDT_HEADER* xsdt = 0;
	UINT32 xsdt_len = sizeof(ACPI_SDT_HEADER) + entries * sizeof(UINT64);
	BS->AllocatePool(EfiACPIReclaimMemory, xsdt_len, (void**)&xsdt);
	if (!xsdt) {
		Print(L"HackBGRT: Failed to allocate memory for XSDT.\n");
		return 0;
	}
	ZeroMem(xsdt, xsdt_len);
	CopyMem(xsdt, xsdt0, min(xsdt0->length, xsdt_len));
	xsdt->length = xsdt_len;
	SetAcpiSdtChecksum(xsdt);
	return xsdt;
}

/**
 * Update the ACPI tables as needed for the desired BGRT change.
 *
 * If action is REMOVE, all BGRT entries will be removed.
 * If action is KEEP, the first BGRT entry will be returned.
 * If action is REPLACE, the given BGRT entry will be stored in each XSDT.
 *
 * @param action The intended action.
 * @param bgrt The BGRT, if action is REPLACE.
 * @return Pointer to the BGRT, or 0 if not found (or destroyed).
 */
static ACPI_BGRT* HandleAcpiTables(enum HackBGRT_action action, ACPI_BGRT* bgrt) {
	for (int i = 0; i < ST->NumberOfTableEntries; i++) {
		EFI_GUID Acpi20TableGuid = ACPI_20_TABLE_GUID;
		EFI_GUID* vendor_guid = &ST->ConfigurationTable[i].VendorGuid;
		if (!CompareGuid(vendor_guid, &AcpiTableGuid) && !CompareGuid(vendor_guid, &Acpi20TableGuid)) {
			continue;
		}
		ACPI_20_RSDP* rsdp = (ACPI_20_RSDP *) ST->ConfigurationTable[i].VendorTable;
		if (CompareMem(rsdp->signature, "RSD PTR ", 8) != 0 || rsdp->revision < 2 || !VerifyAcpiRsdp2Checksums(rsdp)) {
			continue;
		}
		Debug(L"RSDP: revision = %d, OEM ID = %s\n", rsdp->revision, TmpStr(rsdp->oem_id, 6));

		ACPI_SDT_HEADER* xsdt = (ACPI_SDT_HEADER *) (UINTN) rsdp->xsdt_address;
		if (!xsdt || CompareMem(xsdt->signature, "XSDT", 4) != 0 || !VerifyAcpiSdtChecksum(xsdt)) {
			Debug(L"* XSDT: missing or invalid\n");
			continue;
		}
		UINT64* entry_arr = (UINT64*)&xsdt[1];
		UINT32 entry_arr_length = (xsdt->length - sizeof(*xsdt)) / sizeof(UINT64);

		Debug(L"* XSDT: OEM ID = %s, entry count = %d\n", TmpStr(xsdt->oem_id, 6), entry_arr_length);

		int bgrt_count = 0;
		for (int j = 0; j < entry_arr_length; j++) {
			ACPI_SDT_HEADER *entry = (ACPI_SDT_HEADER *)((UINTN)entry_arr[j]);
			if (CompareMem(entry->signature, "BGRT", 4) != 0) {
				continue;
			}
			Debug(L" - ACPI table: %s, revision = %d, OEM ID = %s\n", TmpStr(entry->signature, 4), entry->revision, TmpStr(entry->oem_id, 6));
			switch (action) {
				case HackBGRT_KEEP:
					if (!bgrt) {
						Debug(L" -> Returning this one for later use.\n");
						bgrt = (ACPI_BGRT*) entry;
					}
					break;
				case HackBGRT_REMOVE:
					Debug(L" -> Deleting.\n");
					for (int k = j+1; k < entry_arr_length; ++k) {
						entry_arr[k-1] = entry_arr[k];
					}
					--entry_arr_length;
					entry_arr[entry_arr_length] = 0;
					xsdt->length -= sizeof(entry_arr[0]);
					--j;
					break;
				case HackBGRT_REPLACE:
					Debug(L" -> Replacing.\n");
					entry_arr[j] = (UINTN) bgrt;
			}
			bgrt_count += 1;
		}
		if (!bgrt_count && action == HackBGRT_REPLACE && bgrt) {
			Debug(L" - Adding missing BGRT.\n");
			xsdt = CreateXsdt(xsdt, entry_arr_length + 1);
			entry_arr = (UINT64*)&xsdt[1];
			entry_arr[entry_arr_length++] = (UINTN) bgrt;
			rsdp->xsdt_address = (UINTN) xsdt;
			SetAcpiRsdp2Checksums(rsdp);
		}
		SetAcpiSdtChecksum(xsdt);
	}
	return bgrt;
}

/**
 * Load a PNG image file
 *
 * @param root_dir The root directory for loading a PNG.
 * @param path The PNG path within the root directory.
 * @return The loaded BMP, or 0 if not available.
 */
#include "../my_efilib/my_efilib.h"
#include "../upng/upng.h"

static void* init_bmp(uint32_t w, uint32_t h)
{
	BMP* bmp = 0;

	Debug(L"HackBGRT: init_bmp() (%d x %d).\n", w, h);

	// 3 = RGB 3byte
	// 54 = 24bit BMP has 54byte header
	// Padding for 4 byte alignment
	// const int pad = (w & 3);
	const UINT32 size = ((w * 3) + (w & 3)) * h + 54;
	Debug(L"HackBGRT: init_bmp() AllocatePool %ld.\n", size);
	BS->AllocatePool(EfiBootServicesData, size, (void*)&bmp);
	if (!bmp) return 0;

	// BI_RGB 24bit
	CopyMem(
		bmp,
		"\x42\x4d"
		"\x00\x00\x00\x00"
		"\x00\x00"
		"\x00\x00"
		"\x36\x00\x00\x00"
		"\x28\x00\x00\x00"
		"\x00\x00\x00\x00"
		"\x00\x00\x00\x00"
		"\x01\x00"
		"\x18\x00"
		"\x00\x00\x00\x00"
		"\x00\x00\x00\x00"
		"\x13\x0b\x00\x00"
		"\x13\x0b\x00\x00"
		"\x00\x00\x00\x00"
		"\x00\x00\x00\x00",
		54
	);

	// Intel x86 Only, Byte Order = Little Endian
	bmp->file_size = size;
	bmp->width  = w;
	bmp->height = h;
	bmp->biSizeImage = size - 54;

	return bmp;
}


static void* decode_png(void* buffer, UINTN size)
{
	// upng
	upng_t* upng;
	unsigned width, height, depth;
	unsigned x, y, d;

	upng = upng_new_from_bytes(buffer, size);
	if (!upng) {
		Print(L"HackBGRT: Failed to upng NULL\n");
		return 0;
	}

	if (upng_get_error(upng) != UPNG_EOK) {
		Print(L"HackBGRT: Failed to upng %u %u\n", upng_get_error(upng), upng_get_error_line(upng));
		upng_free(upng);
		return 0;
	}

	// Reads just the header, sets image properties
	if (upng_header(upng) != UPNG_EOK) {
		Print(L"HackBGRT: Failed to upng_header %u %u\n", upng_get_error(upng), upng_get_error_line(upng));
		upng_free(upng);
		return 0;
	}

	// Decodes image data
	if (upng_decode(upng) != UPNG_EOK) {
		Print(L"HackBGRT: Failed to upng_decode %u %u\n", upng_get_error(upng), upng_get_error_line(upng));
		upng_free(upng);
		return 0;
	}

	width  = upng_get_width(upng);
	height = upng_get_height(upng);
	depth  = upng_get_bpp(upng) / 8;

	BMP* bmp = init_bmp(width, height);
	if (!bmp) {
		Print(L"HackBGRT: Failed to init_bmp\n");
		upng_free(upng);
		return 0;
	}

	Debug(L"size: %ux%ux%u (%u)\n", width, height, upng_get_bpp(upng), upng_get_size(upng));
	Debug(L"format: %u\n", upng_get_format(upng));

	if (upng_get_format(upng) == UPNG_RGB8 || upng_get_format(upng) == UPNG_RGBA8) {

		const unsigned char* upng_buffer = upng_get_buffer(upng);
		UINT32 bmp_width = ((width * 3) + (width & 3));
		for (y = 0; y != height; ++y) {
			for (x = 0; x != width; ++x) {
				UINT32 bmp_pos = bmp_width * (height - y - 1) + (x * 3) + 54;
				UINT32 png_pos = (y * width + x) * depth;
				for (d = 0; d < 3; ++d) {
					// B,G,R
					UINT8 c = upng_buffer[png_pos + (depth - d - 1)];
					((UINT8*)bmp)[bmp_pos] = c;
					++bmp_pos;
				}

				// Debug
				if ((x % 32) || (y % 32) || (x > 256) || (y > 256))
					continue;

				// B,G,R
				UINT8 r = ((UINT8*)bmp)[--bmp_pos];
				UINT8 g = ((UINT8*)bmp)[--bmp_pos];
				UINT8 b = ((UINT8*)bmp)[bmp_pos];
				Debug(L"HackBGRT: bmp (%4d, %4d) #%02x%02x%02x.\n", x, y, r, g, b);
			}
		}
	} else {
		Print(L"HackBGRT: HackBGRT Support RGB8 or RGBA8 only (%u)\n", upng_get_format(upng));
		upng_free(upng);
		return 0;
	}

	// Frees the resources attached to a upng_t object
	upng_free(upng);

	return bmp;
}

static BMP* LoadPNG(EFI_FILE_HANDLE root_dir, const CHAR16* path) {
	void* buffer = 0;
	Debug(L"HackBGRT: Loading PNG %s.\n", path);
	UINTN size;
	buffer = LoadFile(root_dir, path, &size);
	if (!buffer) {
		Print(L"HackBGRT: Failed to load PNG (%s)!\n", path);
		BS->Stall(1000000);
		return 0;
	}

	BMP* bmp = decode_png(buffer, size);
	if (!bmp) {
		FreePool(buffer);
		Print(L"HackBGRT: Failed to decoce PNG (%s)!\n", path);
		BS->Stall(1000000);
		return 0;
	}

	return bmp;
}

//------------------------------------------------------------------------------
// jpg2tga.c
// JPEG to TGA file conversion example program.
// Public domain, Rich Geldreich <richgel99@gmail.com>
// Last updated Nov. 26, 2010
//------------------------------------------------------------------------------
#include "../picojpeg/picojpeg.h"

#define STRING(var) #var

#define PJPG_ENUM(DO) \
    DO(PJPEG_OK) \
    DO(PJPG_NO_MORE_BLOCKS) \
    DO(PJPG_BAD_DHT_COUNTS) \
    DO(PJPG_BAD_DHT_INDEX) \
    DO(PJPG_BAD_DHT_MARKER) \
    DO(PJPG_BAD_DQT_MARKER) \
    DO(PJPG_BAD_DQT_TABLE) \
    DO(PJPG_BAD_PRECISION) \
    DO(PJPG_BAD_HEIGHT) \
    DO(PJPG_BAD_WIDTH) \
    DO(PJPG_TOO_MANY_COMPONENTS) \
    DO(PJPG_BAD_SOF_LENGTH) \
    DO(PJPG_BAD_VARIABLE_MARKER) \
    DO(PJPG_BAD_DRI_LENGTH) \
    DO(PJPG_BAD_SOS_LENGTH) \
    DO(PJPG_BAD_SOS_COMP_ID) \
    DO(PJPG_W_EXTRA_BYTES_BEFORE_MARKER) \
    DO(PJPG_NO_ARITHMITIC_SUPPORT) \
    DO(PJPG_UNEXPECTED_MARKER) \
    DO(PJPG_NOT_JPEG) \
    DO(PJPG_UNSUPPORTED_MARKER) \
    DO(PJPG_BAD_DQT_LENGTH) \
    DO(PJPG_TOO_MANY_BLOCKS22) \
    DO(PJPG_UNDEFINED_QUANT_TABLE) \
    DO(PJPG_UNDEFINED_HUFF_TABLE) \
    DO(PJPG_NOT_SINGLE_SCAN) \
    DO(PJPG_UNSUPPORTED_COLORSPACE) \
    DO(PJPG_UNSUPPORTED_SAMP_FACTORS) \
    DO(PJPG_DECODE_ERROR) \
    DO(PJPG_BAD_RESTART_MARKER) \
    DO(PJPG_ASSERTION_ERROR) \
    DO(PJPG_BAD_SOS_SPECTRAL) \
    DO(PJPG_BAD_SOS_SUCCESSIVE) \
    DO(PJPG_STREAM_READ_ERROR) \
    DO(PJPG_NOTENOUGHMEM) \
    DO(PJPG_UNSUPPORTED_COMP_IDENT) \
    DO(PJPG_UNSUPPORTED_QUANT_TABLE) \
    DO(PJPG_UNSUPPORTED_MODE)

#define MAKE_STRINGS(VAR) #VAR,
const char* const PJPG_ERROR_MESSAGE[] = {
    PJPG_ENUM(MAKE_STRINGS)
};

static CHAR16* stringtoC16(const char * const str) {
    static CHAR16 s_tmp[32+1];
    CHAR16* d = s_tmp;
    char* p = (char*)str;
    while (*p) {
        // No Guard on Length Overflow
        *d++ = (CHAR16)*p++;
    }
    *d = 0;
    return s_tmp;
}

//------------------------------------------------------------------------------
#ifndef max
#define max(a,b)    (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a,b)    (((a) < (b)) ? (a) : (b))
#endif
//------------------------------------------------------------------------------
typedef unsigned char uint8;
typedef unsigned int uint;
//------------------------------------------------------------------------------
static unsigned char *g_pInFile;
static uint g_nInFileSize;
static uint g_nInFileOfs;
//------------------------------------------------------------------------------
unsigned char pjpeg_need_bytes_callback(unsigned char* pBuf, unsigned char buf_size, unsigned char *pBytes_actually_read, void *pCallback_data)
{
   uint n;
   pCallback_data;

   n = min(g_nInFileSize - g_nInFileOfs, buf_size);

   if ((g_nInFileOfs < 2048) || ((g_nInFileSize - g_nInFileOfs) < 2048)) {
      Debug(L"pjpeg_need_bytes_callback: buf_size %d, n %d, %d, %d\n", buf_size, n, g_nInFileOfs, g_nInFileSize);
   } else {
      Debug(L".");
   }

   memcpy(pBuf, &g_pInFile[g_nInFileOfs], n);
   *pBytes_actually_read = (unsigned char)(n);
   g_nInFileOfs += n;
   return 0;
}
//------------------------------------------------------------------------------
// Loads JPEG image from specified file. Returns NULL on failure.
// On success, the malloc()'d image's width/height is written to *x and *y, and
// the number of components (1 or 3) is written to *comps.
// pScan_type can be NULL, if not it'll be set to the image's pjpeg_scan_type_t.
// Not thread safe.
// If reduce is non-zero, the image will be more quickly decoded at approximately
// 1/8 resolution (the actual returned resolution will depend on the JPEG
// subsampling factor).
uint8 *pjpeg_load_from_file(void* buffer, UINTN size, int *ix, int *iy, int *comps, pjpeg_scan_type_t *pScan_type, int reduce)
{
   pjpeg_image_info_t image_info;
   int mcu_x = 0;
   int mcu_y = 0;
   uint row_pitch;
   uint8 *pImage;
   uint8 status;
   uint decoded_width, decoded_height;
   uint row_blocks_per_mcu, col_blocks_per_mcu;

   *ix = 0;
   *iy = 0;
   *comps = 0;
   if (pScan_type) *pScan_type = PJPG_GRAYSCALE;

   g_pInFile = (void*)buffer;
   g_nInFileOfs = 0;
   g_nInFileSize = size;

   Debug(L"pjpeg_load_from_file: Size %d.\n", size);
   status = pjpeg_decode_init(&image_info, pjpeg_need_bytes_callback, NULL, (unsigned char)reduce);
   if (status)
   {
      Print(L"pjpeg_decode_init() failed with status %u(%s)\n", status, stringtoC16(PJPG_ERROR_MESSAGE[status]));

      if (status == PJPG_UNSUPPORTED_MODE)
      {
         Print(L"Progressive JPEG files are not supported.\n");
      }

      free(g_pInFile);
      return NULL;
   }

   if (pScan_type)
      *pScan_type = image_info.m_scanType;

   // In reduce mode output 1 pixel per 8x8 block.
   decoded_width = reduce ? (image_info.m_MCUSPerRow * image_info.m_MCUWidth) / 8 : image_info.m_width;
   decoded_height = reduce ? (image_info.m_MCUSPerCol * image_info.m_MCUHeight) / 8 : image_info.m_height;

   // row_pitch = width byte size
   row_pitch = decoded_width * image_info.m_comps;
   pImage = (uint8 *)malloc(row_pitch * decoded_height);
   if (!pImage)
   {
      free(g_pInFile);
      return NULL;
   }

   row_blocks_per_mcu = image_info.m_MCUWidth >> 3;
   col_blocks_per_mcu = image_info.m_MCUHeight >> 3;

   for ( ; ; )
   {
      int y, x;
      uint8 *pDst_row;

      status = pjpeg_decode_mcu();

      if (status)
      {
         if (status != PJPG_NO_MORE_BLOCKS)
         {
            Print(L"pjpeg_decode_mcu() failed with status %u\n", status);

            free(pImage);
            free(g_pInFile);
            return NULL;
         }

         break;
      }

      if (mcu_y >= image_info.m_MCUSPerCol)
      {
         free(pImage);
         free(g_pInFile);
         return NULL;
      }

      if (reduce)
      {
         // In reduce mode, only the first pixel of each 8x8 block is valid.
         pDst_row = pImage + mcu_y * col_blocks_per_mcu * row_pitch + mcu_x * row_blocks_per_mcu * image_info.m_comps;
         if (image_info.m_scanType == PJPG_GRAYSCALE)
         {
            *pDst_row = image_info.m_pMCUBufR[0];
         }
         else
         {
            // uint y, x;
            for (y = 0; y < col_blocks_per_mcu; y++)
            {
               uint src_ofs = (y * 128U);
               for (x = 0; x < row_blocks_per_mcu; x++)
               {
                  pDst_row[0] = image_info.m_pMCUBufR[src_ofs];
                  pDst_row[1] = image_info.m_pMCUBufG[src_ofs];
                  pDst_row[2] = image_info.m_pMCUBufB[src_ofs];
                  pDst_row += 3;
                  src_ofs += 64;
               }

               pDst_row += row_pitch - 3 * row_blocks_per_mcu;
            }
         }
      }
      else
      {
         // Copy MCU's pixel blocks into the destination bitmap.
         pDst_row = pImage + (mcu_y * image_info.m_MCUHeight) * row_pitch + (mcu_x * image_info.m_MCUWidth * image_info.m_comps);

         for (y = 0; y < image_info.m_MCUHeight; y += 8)
         {
            const int by_limit = min(8, image_info.m_height - (mcu_y * image_info.m_MCUHeight + y));

            for (x = 0; x < image_info.m_MCUWidth; x += 8)
            {
               uint8 *pDst_block = pDst_row + x * image_info.m_comps;

               // Compute source byte offset of the block in the decoder's MCU buffer.
               uint src_ofs = (x * 8U) + (y * 16U);
               const uint8 *pSrcR = image_info.m_pMCUBufR + src_ofs;
               const uint8 *pSrcG = image_info.m_pMCUBufG + src_ofs;
               const uint8 *pSrcB = image_info.m_pMCUBufB + src_ofs;

               const int bx_limit = min(8, image_info.m_width - (mcu_x * image_info.m_MCUWidth + x));

               if (image_info.m_scanType == PJPG_GRAYSCALE)
               {
                  int bx, by;
                  for (by = 0; by < by_limit; by++)
                  {
                     uint8 *pDst = pDst_block;

                     for (bx = 0; bx < bx_limit; bx++)
                        *pDst++ = *pSrcR++;

                     pSrcR += (8 - bx_limit);

                     pDst_block += row_pitch;
                  }
               }
               else
               {
                  int bx, by;
                  for (by = 0; by < by_limit; by++)
                  {
                     uint8 *pDst = pDst_block;

                     for (bx = 0; bx < bx_limit; bx++)
                     {
                        pDst[0] = *pSrcR++;
                        pDst[1] = *pSrcG++;
                        pDst[2] = *pSrcB++;
                        pDst += 3;
                     }

                     pSrcR += (8 - bx_limit);
                     pSrcG += (8 - bx_limit);
                     pSrcB += (8 - bx_limit);

                     pDst_block += row_pitch;
                  }
               }
            }

            pDst_row += (row_pitch * 8);
         }
      }

      mcu_x++;
      if (mcu_x == image_info.m_MCUSPerRow)
      {
         mcu_x = 0;
         mcu_y++;
      }
   }

   free(g_pInFile);

   *ix = decoded_width;
   *iy = decoded_height;
   *comps = image_info.m_comps;

   return pImage;
}
//------------------------------------------------------------------------------
static void get_pixel(int* pDst, const uint8 *pSrc, int luma_only, int num_comps)
{
   int r, g, b;
   if (num_comps == 1)
   {
      r = g = b = pSrc[0];
   }
   else if (luma_only)
   {
      const int YR = 19595, YG = 38470, YB = 7471;
      r = g = b = (pSrc[0] * YR + pSrc[1] * YG + pSrc[2] * YB + 32768) / 65536;
   }
   else
   {
      r = pSrc[0]; g = pSrc[1]; b = pSrc[2];
   }
   pDst[0] = r; pDst[1] = g; pDst[2] = b;
}

//------------------------------------------------------------------------------
#define EXIT_FAILURE NULL

static void* decode_jpeg(void* buffer, UINTN size)
{
   int width, height, comps;
   pjpeg_scan_type_t scan_type;
   uint8 *pImage;
   int reduce = 0;
   UINT16 *p = L"";

   pImage = pjpeg_load_from_file(buffer, size, &width, &height, &comps, &scan_type, reduce);
   if (!pImage)
   {
      Print(L"Failed loading source image!\n");
      return EXIT_FAILURE;
   }

   Debug(L"Width: %d, Height: %d, Comps: %d\n", width, height, comps);

   switch (scan_type)
   {
      case PJPG_GRAYSCALE: p = L"GRAYSCALE"; break;
      case PJPG_YH1V1: p = L"H1V1"; break;
      case PJPG_YH2V1: p = L"H2V1"; break;
      case PJPG_YH1V2: p = L"H1V2"; break;
      case PJPG_YH2V2: p = L"H2V2"; break;
   }
   Debug(L"Scan type: %s\n", p);

	BMP* bmp = init_bmp(width, height);
	if (!bmp) {
		Print(L"HackBGRT: Failed to init_bmp\n");
		free(pImage);
		return 0;
	}

	int x, y, d;
	int a[3];
	UINT32 bmp_width = ((width * 3) + (width & 3));
	for (y = 0; y != height; ++y) {
        int pImagePos = (y * width) * comps;
		for (x = 0; x != width; ++x) {
			get_pixel(a, &pImage[pImagePos], (scan_type == PJPG_GRAYSCALE), comps);
            pImagePos += comps;

			// Debug(L"HackBGRT: bmp (%4d, %4d) #%04x.\n", x, y, a[0]);

			UINT32 bmp_pos = bmp_width * (height - y - 1) + (x * 3) + 54;
			for (d = 2; d >= 0; --d) {
				// B,G,R
				UINT8 c = (UINT8)(a[d] & 0xFF);
				((UINT8*)bmp)[bmp_pos] = c;
				++bmp_pos;
			}

			// Debug
			if ((x % 32) || (y % 32) || (x > 256) || (y > 256))
				continue;

			// B,G,R
			UINT8 r = ((UINT8*)bmp)[--bmp_pos];
			UINT8 g = ((UINT8*)bmp)[--bmp_pos];
			UINT8 b = ((UINT8*)bmp)[bmp_pos];
			Debug(L"HackBGRT: bmp (%4d, %4d) #%02x%02x%02x.\n", x, y, r, g, b);
		}
	}

   free(pImage);

   return bmp;
}

static BMP* LoadJPEG(EFI_FILE_HANDLE root_dir, const CHAR16* path) {
    void* buffer = 0;
    Debug(L"HackBGRT: Loading JPEG %s.\n", path);
    UINTN size;
    buffer = LoadFile(root_dir, path, &size);
    if (!buffer) {
        Print(L"HackBGRT: Failed to load JPEG (%s)!\n", path);
        BS->Stall(1000000);
        return 0;
    }

    BMP* bmp = decode_jpeg(buffer, size);
    if (!bmp) {
        FreePool(buffer);
        Print(L"HackBGRT: Failed to decoce JPEG (%s)!\n", path);
        BS->Stall(1000000);
        return 0;
    }

    return bmp;
}

/**
 * Load a bitmap or generate a black one.
 *
 * @param root_dir The root directory for loading a BMP.
 * @param path The BMP path within the root directory; NULL for a black BMP.
 * @return The loaded BMP, or 0 if not available.
 */
static BMP* LoadBMP(EFI_FILE_HANDLE root_dir, const CHAR16* path) {
	BMP* bmp = 0;
	if (!path) {
		bmp = init_bmp(1, 1);
		if (!bmp) {
			Print(L"HackBGRT: Failed to allocate a blank BMP!\n");
			BS->Stall(1000000);
			return 0;
		}
		// Black dot
		CopyMem(
			((UINT8*)bmp)+54,
			"\x00\x00\x00\x00",
			4
		);
		return bmp;
	}
	Debug(L"HackBGRT: Loading %s.\n", path);

	UINTN len = StrLen(path);
	CHAR16 last_char_2 = path[len - 2];
	Debug(L"HackBGRT: Filename Len %d, Last Char %c.\n", (int)len, last_char_2);
	if (last_char_2 == 'm' || last_char_2 == 'M') {
		// xxx.BMP
		bmp = LoadFile(root_dir, path, 0);
	} else if (last_char_2 == 'n' || last_char_2 == 'N') {
		// xxx.PNG
		bmp = LoadPNG(root_dir, path);
	} else {
		// xxx.JPG
		// xxx.JPEG
		bmp = LoadJPEG(root_dir, path);
	}
	if (!bmp) {
		Print(L"HackBGRT: Failed to load BMP (%s)!\n", path);
		BS->Stall(1000000);
		return 0;
	}

	Debug(L"HackBGRT: Load Success %s.\n", path);

	return bmp;
}

/**
 * The main logic for BGRT modification.
 *
 * @param root_dir The root directory for loading a BMP.
 */
void HackBgrt(EFI_FILE_HANDLE root_dir) {
	// REMOVE: simply delete all BGRT entries.
	if (config.action == HackBGRT_REMOVE) {
		HandleAcpiTables(config.action, 0);
		return;
	}

	// KEEP/REPLACE: first get the old BGRT entry.
	ACPI_BGRT* bgrt = HandleAcpiTables(HackBGRT_KEEP, 0);

	// Get the old BMP and position, if possible.
	BMP* old_bmp = 0;
	int old_x = 0, old_y = 0;
	if (bgrt && VerifyAcpiSdtChecksum(bgrt)) {
		old_bmp = (BMP*) (UINTN) bgrt->image_address;
		old_x = bgrt->image_offset_x;
		old_y = bgrt->image_offset_y;
	}

	// Missing BGRT?
	if (!bgrt) {
		// Keep missing = do nothing.
		if (config.action == HackBGRT_KEEP) {
			return;
		}
		// Replace missing = allocate new.
		BS->AllocatePool(EfiACPIReclaimMemory, sizeof(*bgrt), (void**)&bgrt);
		if (!bgrt) {
			Print(L"HackBGRT: Failed to allocate memory for BGRT.\n");
			return;
		}
	}

	// Clear the BGRT.
	const char data[0x38] =
		"BGRT" "\x38\x00\x00\x00" "\x00" "\xd6" "Mtblx*" "HackBGRT"
		"\x20\x17\x00\x00" "PTL " "\x02\x00\x00\x00"
		"\x01\x00" "\x00" "\x00";
	CopyMem(bgrt, data, sizeof(data));

	// Get the image (either old or new).
	BMP* new_bmp = old_bmp;
	if (config.action == HackBGRT_REPLACE) {
		new_bmp = LoadBMP(root_dir, config.image_path);
	}

	// No image = no need for BGRT.
	if (!new_bmp) {
		HandleAcpiTables(HackBGRT_REMOVE, 0);
		return;
	}

	bgrt->image_address = (UINTN) new_bmp;

	// Calculate the automatically centered position for the image.
	int auto_x = 0, auto_y = 0;
	if (GOP()) {
		auto_x = max(0, ((int)GOP()->Mode->Info->HorizontalResolution - (int)new_bmp->width) / 2);
		auto_y = max(0, ((int)GOP()->Mode->Info->VerticalResolution * 2/3 - (int)new_bmp->height) / 2);
	} else if (old_bmp) {
		auto_x = max(0, old_x + ((int)old_bmp->width - (int)new_bmp->width) / 2);
		auto_y = max(0, old_y + ((int)old_bmp->height - (int)new_bmp->height) / 2);
	}

	// Set the position (manual, automatic, original).
	bgrt->image_offset_x = SelectCoordinate(config.image_x, auto_x, old_x);
	bgrt->image_offset_y = SelectCoordinate(config.image_y, auto_y, old_y);
	Debug(L"HackBGRT: BMP at (%d, %d).\n", (int) bgrt->image_offset_x, (int) bgrt->image_offset_y);

	// Store this BGRT in the ACPI tables.
	SetAcpiSdtChecksum(bgrt);
	HandleAcpiTables(HackBGRT_REPLACE, bgrt);
}

/**
 * The main program.
 */
EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *ST_) {
	InitializeLib(image_handle, ST_);

	EFI_LOADED_IMAGE* image;
	if (EFI_ERROR(BS->HandleProtocol(image_handle, &LoadedImageProtocol, (void**) &image))) {
		Debug(L"HackBGRT: LOADED_IMAGE_PROTOCOL failed.\n");
		goto fail;
	}

	EFI_FILE_HANDLE root_dir = LibOpenRoot(image->DeviceHandle);

	CHAR16 **argv;
	int argc = GetShellArgcArgv(image_handle, &argv);

	if (argc <= 1) {
		const CHAR16* config_path = L"\\EFI\\HackBGRT\\config.txt";
		if (!ReadConfigFile(&config, root_dir, config_path)) {
			Print(L"HackBGRT: No config, no command line!\n", config_path);
			goto fail;
		}
	}
	for (int i = 1; i < argc; ++i) {
		ReadConfigLine(&config, root_dir, argv[i]);
	}
	Debug = config.debug ? Print : NullPrint;

	SetResolution(config.resolution_x, config.resolution_y);
	HackBgrt(root_dir);

	EFI_HANDLE next_image_handle = 0;
	if (!config.boot_path) {
		Print(L"HackBGRT: Boot path not specified.\n");
	} else {
		Debug(L"HackBGRT: Loading application %s.\n", config.boot_path);
		EFI_DEVICE_PATH* boot_dp = FileDevicePath(image->DeviceHandle, (CHAR16*) config.boot_path);
		if (EFI_ERROR(BS->LoadImage(0, image_handle, boot_dp, 0, 0, &next_image_handle))) {
			Print(L"HackBGRT: Failed to load application %s.\n", config.boot_path);
		}
	}
	if (!next_image_handle) {
		static CHAR16 default_boot_path[] = L"\\EFI\\HackBGRT\\bootmgfw-original.efi";
		Debug(L"HackBGRT: Loading application %s.\n", default_boot_path);
		EFI_DEVICE_PATH* boot_dp = FileDevicePath(image->DeviceHandle, default_boot_path);
		if (EFI_ERROR(BS->LoadImage(0, image_handle, boot_dp, 0, 0, &next_image_handle))) {
			Print(L"HackBGRT: Also failed to load application %s.\n", default_boot_path);
			goto fail;
		}
		Print(L"HackBGRT: Reverting to %s.\n", default_boot_path);
		Print(L"Press escape to cancel, any other key to boot.\n");
		if (ReadKey().ScanCode == SCAN_ESC) {
			goto fail;
		}
		config.boot_path = default_boot_path;
	}
	if (config.debug) {
		Print(L"HackBGRT: Ready to boot.\nPress escape to cancel, any other key to boot.\n");
		if (ReadKey().ScanCode == SCAN_ESC) {
			return 0;
		}
	}
	if (EFI_ERROR(BS->StartImage(next_image_handle, 0, 0))) {
		Print(L"HackBGRT: Failed to start %s.\n", config.boot_path);
		goto fail;
	}
	Print(L"HackBGRT: Started %s. Why are we still here?!\n", config.boot_path);
	goto fail;

	fail: {
		Print(L"HackBGRT has failed. Use parameter debug=1 for details.\n");
		Print(L"Get a Windows install disk or a recovery disk to fix your boot.\n");
		#ifdef GIT_DESCRIBE
			Print(L"HackBGRT version: " GIT_DESCRIBE L"\n");
		#else
			Print(L"HackBGRT version: unknown; not an official release?\n");
		#endif
		Print(L"Press any key to exit.\n");
		ReadKey();
		return 1;
	}
}

/**
 * Forward to EfiMain.
 *
 * Some compilers and architectures differ in underscore handling. This helps.
 */
// EFI_STATUS EFIAPI _EfiMain(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *ST_) {
// 	return EfiMain(image_handle, ST_);
// }
