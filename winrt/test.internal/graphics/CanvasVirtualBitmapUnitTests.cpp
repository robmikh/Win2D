// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#include "pch.h"

#include <lib/images/CanvasVirtualBitmap.h>

#include "../mocks/MockD2DImageSource.h"
#include "../mocks/MockD2DImageSourceFromWic.h"
#include "../mocks/MockD2DTransformedImageSource.h"
#include "../mocks/MockWICBitmapSource.h"

#if WINVER > _WIN32_WINNT_WINBLUE

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

static HRESULT const AnyHR = 0x81234567;

class TestVirtualBitmapAdapter : public CanvasVirtualBitmapAdapter
{
public:
    virtual ComPtr<IAsyncAction> RunAsync(std::function<void()>&& fn) override
    {
        HRESULT hr = ExceptionBoundary([&] { fn(); });

        auto action = Make<MockAsyncAction>();
        action->SetResult(hr);

        return action;
    }
};

TEST_CLASS(CanvasVirtualBitmapUnitTest)
{

    //
    // CanvasVirtualBitmapFactory has no test coverage here (as with
    // CanvasBitmapFactory).  The factory methods all boil down to calling
    // CanvasVirtualBitmap::CreateNew with an appropriate IWICBitmapSource /
    // transform.  test.external contains some tests to verify that these
    // roughly do the right thing.
    //
    // CanvasVirtualBitmap.GetBounds is tested in test.external since it just
    // calls through to GetImageBoundsImpl.
    //

    struct Fixture
    {
        std::shared_ptr<TestVirtualBitmapAdapter> Adapter;

        ComPtr<StubD2DDeviceContext> DeviceContext;
        WicBitmapSource Bitmap;

        ComPtr<CanvasDevice> Device;

        D2D1_RECT_F D2DBounds;

        Fixture(D2D1_RECT_F bounds = { 10, 20, 30, 40 })
            : Adapter(std::make_shared<TestVirtualBitmapAdapter>())
            , Bitmap{ Make<MockWICBitmapSource>(), WICBitmapTransformRotate0 }
            , D2DBounds(bounds)
        {
            CanvasVirtualBitmapAdapter::SetInstance(Adapter);

            auto d3dDevice = Make<StubD3D11Device>();
            auto d2dDevice = Make<MockD2DDevice>();

            DeviceContext = Make<StubD2DDeviceContext>(d2dDevice.Get());

            DeviceContext->GetImageLocalBoundsMethod.SetExpectedCalls(0);

            d2dDevice->MockCreateDeviceContext =
                [=] (auto, auto** deviceContext)
                {
                    ThrowIfFailed(DeviceContext.CopyTo(deviceContext));
                };

            Device = Make<CanvasDevice>(d2dDevice.Get(), d3dDevice.Get());
        }

        ComPtr<MockD2DImageSourceFromWic> ExpectCreateImageSourceFromWic(
            D2D1_IMAGE_SOURCE_LOADING_OPTIONS expectedLoadingOptions,
            D2D1_ALPHA_MODE expectedAlphaMode)
        {
            auto imageSource = Make<MockD2DImageSourceFromWic>();

            DeviceContext->CreateImageSourceFromWicMethod.SetExpectedCalls(1,
                [=] (IWICBitmapSource* bitmapSource, D2D1_IMAGE_SOURCE_LOADING_OPTIONS loadingOptions, D2D1_ALPHA_MODE alphaMode, ID2D1ImageSourceFromWic** result)
                {
                    Assert::IsTrue(IsSameInstance(Bitmap.Source.Get(), bitmapSource));
                    Assert::AreEqual<uint32_t>(expectedLoadingOptions, loadingOptions);
                    Assert::AreEqual(expectedAlphaMode, alphaMode, L"Alpha Mode");
                    return imageSource.CopyTo(result);
                });

            DeviceContext->GetImageLocalBoundsMethod.SetExpectedCalls(1,
                [=] (ID2D1Image*, D2D1_RECT_F* bounds)
                {
                    *bounds = D2DBounds;
                    return S_OK;
                });

            return imageSource;
        }

        ComPtr<CanvasVirtualBitmap> CreateVirtualBitmap(CanvasVirtualBitmapOptions options, CanvasAlphaMode alphaMode)
        {
            return CanvasVirtualBitmap::CreateNew(
                Device.Get(),
                Bitmap,
                options,
                alphaMode);
        }

        Fixture(Fixture const&) = delete;
        Fixture& operator=(Fixture const&) = delete;
    };

    TEST_METHOD_EX(CanvasVirtualBitmap_CreateNew_Untransformed_CallsThrough)
    {
        Fixture f;

        auto imageSource = f.ExpectCreateImageSourceFromWic(D2D1_IMAGE_SOURCE_LOADING_OPTIONS_NONE, D2D1_ALPHA_MODE_PREMULTIPLIED);

        auto virtualBitmap = f.CreateVirtualBitmap(CanvasVirtualBitmapOptions::None, CanvasAlphaMode::Premultiplied);

        auto actualImageSource = GetWrappedResource<ID2D1ImageSource>(virtualBitmap.Get());
        Assert::IsTrue(IsSameInstance(imageSource.Get(), actualImageSource.Get()));

        ComPtr<ICanvasDevice> actualDevice;
        ThrowIfFailed(virtualBitmap->get_Device(&actualDevice));
        Assert::IsTrue(IsSameInstance(f.Device.Get(), actualDevice.Get()));
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_CreateNew_PassesCorrectAlphaModeThrough)
    {
        for (auto alphaMode : { CanvasAlphaMode::Premultiplied, CanvasAlphaMode::Ignore })
        {
            Fixture f;

            f.ExpectCreateImageSourceFromWic(D2D1_IMAGE_SOURCE_LOADING_OPTIONS_NONE, ToD2DAlphaMode(alphaMode));
            f.CreateVirtualBitmap(CanvasVirtualBitmapOptions::None, alphaMode);
        }
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_CreateNew_FailsWithGoodError_WhenPassedAlphaModeStraight)
    {
        Fixture f;

        ExpectHResultException(E_INVALIDARG,
            [&] { f.CreateVirtualBitmap(CanvasVirtualBitmapOptions::None, CanvasAlphaMode::Straight); });

        ValidateStoredErrorState(E_INVALIDARG, Strings::InvalidAlphaModeForImageSource);
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_CreateNew_TransformsAreApplied)
    {
        struct TestCase
        {
            WICBitmapTransformOptions Transform;
            D2D1_ORIENTATION Orientation;
        };

        auto transpose = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        auto transverse = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);

        TestCase cases[] =
        {
            { WICBitmapTransformRotate270,      D2D1_ORIENTATION_ROTATE_CLOCKWISE270                 },
            { WICBitmapTransformRotate180,      D2D1_ORIENTATION_ROTATE_CLOCKWISE180                 },
            { WICBitmapTransformRotate90,       D2D1_ORIENTATION_ROTATE_CLOCKWISE90                  },
            { WICBitmapTransformFlipHorizontal, D2D1_ORIENTATION_FLIP_HORIZONTAL                     },
            { WICBitmapTransformFlipVertical,   D2D1_ORIENTATION_ROTATE_CLOCKWISE180_FLIP_HORIZONTAL },
            { transpose,                        D2D1_ORIENTATION_ROTATE_CLOCKWISE90_FLIP_HORIZONTAL  },
            { transverse,                       D2D1_ORIENTATION_ROTATE_CLOCKWISE270_FLIP_HORIZONTAL },
        };

        for (auto testCase : cases)
        {
            Fixture f;
            f.Bitmap.Transform = testCase.Transform;

            auto imageSource = f.ExpectCreateImageSourceFromWic(D2D1_IMAGE_SOURCE_LOADING_OPTIONS_NONE, D2D1_ALPHA_MODE_PREMULTIPLIED);

            auto transformedImageSource = Make<MockD2DTransformedImageSource>();

            f.DeviceContext->CreateTransformedImageSourceMethod.SetExpectedCalls(1,
                [=] (auto source, auto properties, auto result)
                {
                    Assert::IsTrue(IsSameInstance(imageSource.Get(), source));
                    Assert::AreEqual<uint32_t>(testCase.Orientation, properties->orientation);
                    Assert::AreEqual(1.0f, properties->scaleX);
                    Assert::AreEqual(1.0f, properties->scaleY);
                    Assert::AreEqual(D2D1_INTERPOLATION_MODE_LINEAR, properties->interpolationMode);
                    Assert::AreEqual<uint32_t>(D2D1_TRANSFORMED_IMAGE_SOURCE_OPTIONS_NONE, properties->options);
                    return transformedImageSource.CopyTo(result);
                });

            auto virtualBitmap = f.CreateVirtualBitmap(CanvasVirtualBitmapOptions::None, CanvasAlphaMode::Premultiplied);
            auto actualImageSource = GetWrappedResource<ID2D1TransformedImageSource>(virtualBitmap.Get());
            Assert::IsTrue(IsSameInstance(transformedImageSource.Get(), actualImageSource.Get()));
        }
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_CreateNew_WhenReleaseSourceSet_ImageSourceIsCreatedWithReleaseSource)
    {
        Fixture f;

        auto imageSource = f.ExpectCreateImageSourceFromWic(D2D1_IMAGE_SOURCE_LOADING_OPTIONS_RELEASE_SOURCE, D2D1_ALPHA_MODE_PREMULTIPLIED);
        auto virtualBitmap = f.CreateVirtualBitmap(CanvasVirtualBitmapOptions::ReleaseSource, CanvasAlphaMode::Premultiplied);
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_CreateNew_WhenCacheOnDemandSet_AndImageDoesNotSupportIndexing_ImageSourceIsCreatedWithNoLoadingOptions)
    {
        Fixture f;
        f.Bitmap.Indexed = false;

        auto imageSource = f.ExpectCreateImageSourceFromWic(D2D1_IMAGE_SOURCE_LOADING_OPTIONS_NONE, D2D1_ALPHA_MODE_PREMULTIPLIED);
        auto virtualBitmap = f.CreateVirtualBitmap(CanvasVirtualBitmapOptions::CacheOnDemand, CanvasAlphaMode::Premultiplied);
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_CreateNew_WhenCacheOnDemandSet_AndImageSupportsIndexing_ImageSourceIsCreatedWithCacheOnDemand)
    {
        Fixture f;
        f.Bitmap.Indexed = true;

        auto imageSource = f.ExpectCreateImageSourceFromWic(D2D1_IMAGE_SOURCE_LOADING_OPTIONS_CACHE_ON_DEMAND, D2D1_ALPHA_MODE_PREMULTIPLIED);
        auto virtualBitmap = f.CreateVirtualBitmap(CanvasVirtualBitmapOptions::CacheOnDemand, CanvasAlphaMode::Premultiplied);
    }


    struct CreatedFixture : public Fixture
    {
        ComPtr<MockD2DImageSourceFromWic> ImageSource;
        ComPtr<CanvasVirtualBitmap> VirtualBitmap;

        CreatedFixture()
            : ImageSource(ExpectCreateImageSourceFromWic(D2D1_IMAGE_SOURCE_LOADING_OPTIONS_NONE, D2D1_ALPHA_MODE_PREMULTIPLIED))
            , VirtualBitmap(CreateVirtualBitmap(CanvasVirtualBitmapOptions::None, CanvasAlphaMode::Premultiplied))
        {
        }
    };

    TEST_METHOD_EX(CanvasVirtualBitmap_GetD2DImage)
    {
        CreatedFixture f;

        float realizedDpi = 0;
        auto d2dImage = f.VirtualBitmap->GetD2DImage(nullptr, nullptr, GetImageFlags::None, 0.0f, &realizedDpi);

        Assert::IsTrue(IsSameInstance(f.ImageSource.Get(), d2dImage.Get()));
        Assert::AreEqual(96.0f, realizedDpi);
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_SizeProperties)
    {
        CreatedFixture f;

        Size expectedSize { f.D2DBounds.right - f.D2DBounds.left, f.D2DBounds.bottom - f.D2DBounds.top };

        BitmapSize bitmapSize;
        ThrowIfFailed(f.VirtualBitmap->get_SizeInPixels(&bitmapSize));
        Assert::AreEqual(expectedSize.Width, (float)bitmapSize.Width);
        Assert::AreEqual(expectedSize.Height, (float)bitmapSize.Height);

        Size size;
        ThrowIfFailed(f.VirtualBitmap->get_Size(&size));
        Assert::AreEqual(expectedSize, size);

        Rect bounds;
        ThrowIfFailed(f.VirtualBitmap->get_Bounds(&bounds));
        Assert::AreEqual(FromD2DRect(f.D2DBounds), bounds);
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_CacheMethods_FailWhenCreatedWithoutCacheOnDemandSet)
    {
        CreatedFixture f;

        ComPtr<IAsyncAction> a;
        Rect anyRegion{};

        std::function<HRESULT()> fns[] =
        {
            [&] { return f.VirtualBitmap->EnsureCachedAsync(&a); },
            [&] { return f.VirtualBitmap->EnsureCachedAsyncWithRegion(anyRegion, &a); },
            [&] { return f.VirtualBitmap->TrimCache(); },
            [&] { return f.VirtualBitmap->TrimCacheWithRegion(anyRegion); }
        };

        for (auto fn : fns)
        {
            Assert::AreEqual(E_FAIL, fn());
            ValidateStoredErrorState(E_FAIL, Strings::CacheOnDemandNotSet);
        }
    }

    struct CacheOnDemandFixture : public Fixture
    {
        ComPtr<MockD2DImageSourceFromWic> ImageSource;
        ComPtr<CanvasVirtualBitmap> VirtualBitmap;

        CacheOnDemandFixture(WICBitmapTransformOptions transform, D2D1_RECT_F imageBounds = { 10, 20, 30, 40 })
            : Fixture(imageBounds)
        {
            Bitmap.Transform = transform;
            Bitmap.Indexed = true;

            ImageSource = ExpectCreateImageSourceFromWic(D2D1_IMAGE_SOURCE_LOADING_OPTIONS_CACHE_ON_DEMAND, D2D1_ALPHA_MODE_PREMULTIPLIED);

            if (transform != WICBitmapTransformRotate0)
            {
                DeviceContext->CreateTransformedImageSourceMethod.SetExpectedCalls(1,
                    [=] (auto, auto, auto result)
                    {
                        return Make<MockD2DTransformedImageSource>().CopyTo(result);
                    });
            }

            VirtualBitmap = CreateVirtualBitmap(CanvasVirtualBitmapOptions::CacheOnDemand, CanvasAlphaMode::Premultiplied);
        }
    };

    template<typename T>
    void TestCacheOnDemand(T fn)
    {
        // Validate the various transforms and that the regions are pass through
        // correctly.
        //
        // All the cases are called with Rect{ 10, 20, 30, 40 } passed as the
        // region.  The transform needs to be unapplied when calling through
        // D2D.
        struct TestCase
        {
            WICBitmapTransformOptions Transform;
            D2D1_RECT_U ExpectedRegion;
        };

        Rect passedRegion{ 110, 120, 10, 20 };
        D2D1_RECT_F imageBounds{ 100, 100, 200, 200 };

        // Transforms are around the image center, which is (150, 150)
        //
        // Passed region relative to center is ((-40, -30) - (-30, -10)

        auto transpose = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
        auto transverse = static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);

        TestCase testCases[]
        {
            // TRANSFORM                        (RELATIVE) + (150, 150)   = ABSOLUTE ltrb
            { WICBitmapTransformRotate0,        /* (-40, -30) (-30, -10) */ { 110, 120, 120, 140 } },
            { WICBitmapTransformRotate270,      /* (-30,  40) (-10,  30) */ { 120, 190, 140, 180 } },
            { WICBitmapTransformRotate180,      /* ( 40,  30) ( 30,  10) */ { 190, 180, 180, 160 } },
            { WICBitmapTransformRotate90,       /* ( 30, -40) ( 10, -30) */ { 180, 110, 160, 120 } },
            { WICBitmapTransformFlipHorizontal, /* ( 40, -30) ( 30, -10) */ { 190, 120, 180, 140 } },
            { WICBitmapTransformFlipVertical,   /* (-40,  30) (-30,  10) */ { 110, 180, 120, 160 } },
            { transpose,                        /* ( 30,  40) ( 10   30) */ { 180, 190, 160, 180 } },
            { transverse,                       /* (-30, -40) (-10, -30) */ { 120, 110, 140, 120 } }
        };

        for (auto t : testCases)
        {
            CacheOnDemandFixture f(t.Transform, imageBounds);
            fn(f, passedRegion, t.ExpectedRegion);
        }
    }


    TEST_METHOD_EX(CanvasVirtualBitmap_EnsureCachedAsync_CallsThrough)
    {
        TestCacheOnDemand(
            [] (auto& f, auto, auto)
            {
                f.ImageSource->EnsureCachedMethod.SetExpectedCalls(1,
                    [] (auto r)
                    {
                        Assert::IsNull(r);
                        return AnyHR;
                    });

                ComPtr<IAsyncAction> action;
                ThrowIfFailed(f.VirtualBitmap->EnsureCachedAsync(&action));

                HRESULT errorCode;
                ThrowIfFailed(As<IAsyncInfo>(action)->get_ErrorCode(&errorCode));

                Assert::AreEqual(AnyHR, errorCode);
            });
    }


    TEST_METHOD_EX(CanvasVirtualBitmap_EnsureCachedAsyncWithRegion_CallsThrough)
    {
        TestCacheOnDemand(
            [] (auto& f, auto region, auto expectedRegion)
            {
                f.ImageSource->EnsureCachedMethod.SetExpectedCalls(1,
                    [=] (auto r)
                    {
                        Assert::AreEqual(expectedRegion, *r);
                        return AnyHR;
                    });

                ComPtr<IAsyncAction> action;
                ThrowIfFailed(f.VirtualBitmap->EnsureCachedAsyncWithRegion(region, &action));

                HRESULT errorCode;
                ThrowIfFailed(As<IAsyncInfo>(action)->get_ErrorCode(&errorCode));

                Assert::AreEqual(AnyHR, errorCode);
            });
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_TrimCache_CallsThrough)
    {
        TestCacheOnDemand(
            [] (auto& f, auto, auto)
            {
                f.ImageSource->TrimCacheMethod.SetExpectedCalls(1,
                    [] (auto r)
                    {
                        Assert::IsNull(r);
                        return AnyHR;
                    });

                Assert::AreEqual(AnyHR, f.VirtualBitmap->TrimCache());
            });
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_TrimCacheWithRegion_CallsThrough)
    {
        TestCacheOnDemand(
            [] (auto& f, auto region, auto expectedRegion)
            {
                f.ImageSource->TrimCacheMethod.SetExpectedCalls(1,
                    [=] (auto r)
                    {
                        Assert::AreEqual(expectedRegion, *r);
                        return AnyHR;
                    });

                Assert::AreEqual(AnyHR, f.VirtualBitmap->TrimCacheWithRegion(region));
            });
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_IsCachedOnDemand_UsesEnsureCachedToDetermineResult)
    {
        std::pair<HRESULT, boolean> testCases[]
        {
            { S_OK                         , TRUE },
            { D2DERR_WRONG_STATE           , TRUE },
            { D2DERR_UNSUPPORTED_OPERATION , FALSE },
            { E_FAIL                       , TRUE }
        };

        for (auto testCase : testCases)
        {
            CreatedFixture f;
            f.ImageSource->EnsureCachedMethod.SetExpectedCalls(1,
                [=] (auto r)
                {
                    Assert::AreEqual(D2D1_RECT_U{}, *r);
                    return testCase.first;
                });

            boolean value;
            ThrowIfFailed(f.VirtualBitmap->get_IsCachedOnDemand(&value));

            Assert::AreEqual(testCase.second, value);
        }
    }

    //
    // In the interop case, various properties of the interop'd image source
    // need to be obtained by probing the passed in resource.  We don't have
    // direct ways to query for these, but also don't want to exhaustively
    // retest all methods and properties.  Instead we pick a representative set
    // of things to test.
    //

    struct InteropFixture
    {
        ComPtr<StubD2DDeviceContext> DeviceContext;
        ComPtr<CanvasDevice> Device;

        InteropFixture()
        {
            auto d3dDevice = Make<StubD3D11Device>();
            auto d2dDevice = Make<MockD2DDevice>();

            DeviceContext = Make<StubD2DDeviceContext>(d2dDevice.Get());
            DeviceContext->GetImageLocalBoundsMethod.SetExpectedCalls(0);

            d2dDevice->MockCreateDeviceContext =
                [=] (auto, auto** deviceContext)
                {
                    ThrowIfFailed(DeviceContext.CopyTo(deviceContext));
                };

            Device = Make<CanvasDevice>(d2dDevice.Get(), d3dDevice.Get());
        }
    };

    TEST_METHOD_EX(CanvasVirtualBitmap_Interop_ImageSourceFromWic)
    {
        InteropFixture f;

        auto imageSource = Make<MockD2DImageSourceFromWic>();

        f.DeviceContext->GetImageLocalBoundsMethod.SetExpectedCalls(1,
            [=] (ID2D1Image* img, D2D1_RECT_F* bounds)
            {
                Assert::IsTrue(IsSameInstance(imageSource.Get(), img));
                *bounds = D2D1_RECT_F{ 10, 20, 30, 40 };
                return S_OK;
            });

        auto virtualBitmap = ResourceManager::GetOrCreate<ICanvasVirtualBitmap>(f.Device.Get(), imageSource.Get());

        // Bounds was retrieved
        Rect bounds;
        ThrowIfFailed(virtualBitmap->get_Bounds(&bounds));
        Assert::AreEqual(Rect{ 10, 20, 20, 20 }, bounds);

        // The imageSourceFromWic was retrieved
        imageSource->EnsureCachedMethod.SetExpectedCalls(1);
        boolean value;
        ThrowIfFailed(virtualBitmap->get_IsCachedOnDemand(&value));
        Assert::IsTrue(!!value);

        // The orientation was inferred as default
        imageSource->TrimCacheMethod.SetExpectedCalls(1,
            [] (auto r)
            {
                Assert::AreEqual(D2D1_RECT_U{ 10, 20, 12, 22 }, *r);
                return S_OK;
            });
        ThrowIfFailed(virtualBitmap->TrimCacheWithRegion(Rect{ 10, 20, 2, 2 }));
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_Interop_TransformedImageSource)
    {
        InteropFixture f;

        auto transformedImageSource = Make<MockD2DTransformedImageSource>();

        transformedImageSource->GetPropertiesMethod.SetExpectedCalls(1,
            [] (auto p)
            {
                *p = D2D1_TRANSFORMED_IMAGE_SOURCE_PROPERTIES{};
                p->orientation = D2D1_ORIENTATION_FLIP_HORIZONTAL;
            });

        auto imageSourceFromWic = Make<MockD2DImageSourceFromWic>();
        transformedImageSource->GetSourceMethod.SetExpectedCalls(1,
            [=] (auto v)
            {
                return imageSourceFromWic.CopyTo(v);
            });

        f.DeviceContext->GetImageLocalBoundsMethod.SetExpectedCalls(1,
            [=] (ID2D1Image* img, D2D1_RECT_F* bounds)
            {
                Assert::IsTrue(IsSameInstance(transformedImageSource.Get(), img));
                *bounds = D2D1_RECT_F{ 10, 20, 30, 40 };
                return S_OK;
            });

        auto virtualBitmap = ResourceManager::GetOrCreate<ICanvasVirtualBitmap>(f.Device.Get(), transformedImageSource.Get());

        // Bounds was retrieved
        Rect bounds;
        ThrowIfFailed(virtualBitmap->get_Bounds(&bounds));
        Assert::AreEqual(Rect{ 10, 20, 20, 20 }, bounds);

        // The imageSourceFromWic was retrieved
        imageSourceFromWic->EnsureCachedMethod.SetExpectedCalls(1);
        boolean value;
        ThrowIfFailed(virtualBitmap->get_IsCachedOnDemand(&value));
        Assert::IsTrue(!!value);

        // The orientation was picked up from the properties
        imageSourceFromWic->TrimCacheMethod.SetExpectedCalls(1,
            [] (auto r)
            {
                Assert::AreEqual(D2D1_RECT_U{ 30, 20, 28, 22 }, *r);
                return S_OK;
            });
        ThrowIfFailed(virtualBitmap->TrimCacheWithRegion(Rect{ 10, 20, 2, 2 }));
    }

    TEST_METHOD_EX(CanvasVirtualBitmap_Interop_UnknownImageSource)
    {
        InteropFixture f;

        // For example, this might be created via CreateImageSourceFromDxgi.
        auto unknownImageSource = Make<MockD2DImageSource>();

        f.DeviceContext->GetImageLocalBoundsMethod.SetExpectedCalls(1,
            [=] (ID2D1Image* img, D2D1_RECT_F* bounds)
            {
                Assert::IsTrue(IsSameInstance(unknownImageSource.Get(), img));
                *bounds = D2D1_RECT_F{ 10, 20, 30, 40 };
                return S_OK;
            });

        auto virtualBitmap = ResourceManager::GetOrCreate<ICanvasVirtualBitmap>(f.Device.Get(), unknownImageSource.Get());

        // Bounds was retrieved
        Rect bounds;
        ThrowIfFailed(virtualBitmap->get_Bounds(&bounds));
        Assert::AreEqual(Rect{ 10, 20, 20, 20 }, bounds);

        // The imageSourceFromWic was not retrieved, so methods that need it fail
        boolean value;
        ComPtr<IAsyncAction> action;
        Assert::AreEqual(E_FAIL, virtualBitmap->get_IsCachedOnDemand(&value));
        Assert::AreEqual(E_FAIL, virtualBitmap->EnsureCachedAsync(&action));
        Assert::AreEqual(E_FAIL, virtualBitmap->EnsureCachedAsyncWithRegion(Rect{}, &action));
        Assert::AreEqual(E_FAIL, virtualBitmap->TrimCache());
        Assert::AreEqual(E_FAIL, virtualBitmap->TrimCacheWithRegion(Rect{}));
    }
};

//
// TODO 6063: transformed image sources may have scale as well as orientation.
// When adding support for custom DPI we'll need to account for this.
//

#endif