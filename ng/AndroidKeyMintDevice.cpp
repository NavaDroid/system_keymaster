/*
 * Copyright 2020, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "android.hardware.security.keymint-impl"
#include <android-base/logging.h>

#include "AndroidKeyMintDevice.h"

#include <aidl/android/hardware/security/keymint/ErrorCode.h>

#include <keymaster/android_keymaster.h>
#include <keymaster/contexts/pure_soft_keymaster_context.h>
#include <keymaster/keymaster_configuration.h>

#include "AndroidKeyMintOperation.h"
#include "KeyMintUtils.h"

namespace aidl::android::hardware::security::keymint {

using namespace ::keymaster;
using namespace km_utils;

namespace {

vector<KeyCharacteristics> convertKeyCharacteristics(SecurityLevel keyMintSecurityLevel,
                                                     const AuthorizationSet& sw_enforced,
                                                     const AuthorizationSet& hw_enforced) {
    KeyCharacteristics enforced;

    enforced.securityLevel = keyMintSecurityLevel;
    if (keyMintSecurityLevel != SecurityLevel::SOFTWARE) {
        // We're pretending to be TRUSTED_ENVIRONMENT or STRONGBOX.  Only the entries in hw_enforced
        // should be returned.
        enforced.authorizations = kmParamSet2Aidl(hw_enforced);
        KeyCharacteristics enforced{keyMintSecurityLevel, kmParamSet2Aidl(hw_enforced)};
        return {std::move(enforced)};
    }

    CHECK(hw_enforced.empty()) << "Hardware-enforced list is non-empty for pure SW KeyMint";

    // This is a pure software implementation, so all tags are in sw_enforced.  We need to walk
    // through the SW-enforced list and figure out which tags to return and which not.

    for (auto& entry : sw_enforced) {
        switch (entry.tag) {
        /* Invalid and unused */
        case KM_TAG_ECIES_SINGLE_HASH_MODE:
        case KM_TAG_INVALID:
        case KM_TAG_KDF:
            CHECK(false) << "We shouldn't see tag " << entry.tag;
            break;

        /* Unimplemented */
        case KM_TAG_ALLOW_WHILE_ON_BODY:
        case KM_TAG_APPLICATION_ID:
        case KM_TAG_BOOTLOADER_ONLY:
        case KM_TAG_EARLY_BOOT_ONLY:
        case KM_TAG_ROLLBACK_RESISTANCE:
        case KM_TAG_ROLLBACK_RESISTANT:
        case KM_TAG_STORAGE_KEY:
            break;

        /* Unenforceable */
        case KM_TAG_ACTIVE_DATETIME:
        case KM_TAG_ALL_APPLICATIONS:
        case KM_TAG_ALL_USERS:
        case KM_TAG_CREATION_DATETIME:
        case KM_TAG_ORIGINATION_EXPIRE_DATETIME:
        case KM_TAG_TRUSTED_CONFIRMATION_REQUIRED:
        case KM_TAG_TRUSTED_USER_PRESENCE_REQUIRED:
        case KM_TAG_USAGE_EXPIRE_DATETIME:
        case KM_TAG_USER_ID:
            break;

        /* Disallowed in KeyCharacteristics */
        case KM_TAG_APPLICATION_DATA:
            break;

        /* Not key characteristics */
        case KM_TAG_ASSOCIATED_DATA:
        case KM_TAG_ATTESTATION_APPLICATION_ID:
        case KM_TAG_ATTESTATION_CHALLENGE:
        case KM_TAG_ATTESTATION_ID_BRAND:
        case KM_TAG_ATTESTATION_ID_DEVICE:
        case KM_TAG_ATTESTATION_ID_IMEI:
        case KM_TAG_ATTESTATION_ID_MANUFACTURER:
        case KM_TAG_ATTESTATION_ID_MEID:
        case KM_TAG_ATTESTATION_ID_MODEL:
        case KM_TAG_ATTESTATION_ID_PRODUCT:
        case KM_TAG_ATTESTATION_ID_SERIAL:
        case KM_TAG_AUTH_TOKEN:
        case KM_TAG_CERTIFICATE_SERIAL:
        case KM_TAG_CERTIFICATE_SUBJECT:
        case KM_TAG_CONFIRMATION_TOKEN:
        case KM_TAG_DEVICE_UNIQUE_ATTESTATION:
        case KM_TAG_IDENTITY_CREDENTIAL_KEY:
        case KM_TAG_MAC_LENGTH:
        case KM_TAG_NONCE:
        case KM_TAG_RESET_SINCE_ID_ROTATION:
        case KM_TAG_ROOT_OF_TRUST:
        case KM_TAG_UNIQUE_ID:
            break;

        /* Enforced */
        case KM_TAG_ALGORITHM:
        case KM_TAG_AUTH_TIMEOUT:
        case KM_TAG_BLOB_USAGE_REQUIREMENTS:
        case KM_TAG_BLOCK_MODE:
        case KM_TAG_BOOT_PATCHLEVEL:
        case KM_TAG_CALLER_NONCE:
        case KM_TAG_DIGEST:
        case KM_TAG_EC_CURVE:
        case KM_TAG_EXPORTABLE:
        case KM_TAG_INCLUDE_UNIQUE_ID:
        case KM_TAG_KEY_SIZE:
        case KM_TAG_MAX_USES_PER_BOOT:
        case KM_TAG_MIN_MAC_LENGTH:
        case KM_TAG_MIN_SECONDS_BETWEEN_OPS:
        case KM_TAG_NO_AUTH_REQUIRED:
        case KM_TAG_ORIGIN:
        case KM_TAG_OS_PATCHLEVEL:
        case KM_TAG_OS_VERSION:
        case KM_TAG_PADDING:
        case KM_TAG_PURPOSE:
        case KM_TAG_RSA_OAEP_MGF_DIGEST:
        case KM_TAG_RSA_PUBLIC_EXPONENT:
        case KM_TAG_UNLOCKED_DEVICE_REQUIRED:
        case KM_TAG_USER_AUTH_TYPE:
        case KM_TAG_USER_SECURE_ID:
        case KM_TAG_VENDOR_PATCHLEVEL:
            enforced.authorizations.push_back(kmParam2Aidl(entry));
        }
    }

    return {std::move(enforced)};
}

Certificate convertCertificate(const keymaster_blob_t& cert) {
    return {std::vector<uint8_t>(cert.data, cert.data + cert.data_length)};
}

vector<Certificate> convertCertificateChain(const CertificateChain& chain) {
    vector<Certificate> retval;
    retval.reserve(chain.entry_count);
    std::transform(chain.begin(), chain.end(), std::back_inserter(retval), convertCertificate);
    return retval;
}

}  // namespace

constexpr size_t kOperationTableSize = 16;

AndroidKeyMintDevice::AndroidKeyMintDevice(SecurityLevel securityLevel)
    : impl_(new ::keymaster::AndroidKeymaster(
          [&]() -> auto {
              auto context = new PureSoftKeymasterContext(
                  KmVersion::KEYMINT_1, static_cast<keymaster_security_level_t>(securityLevel));
              context->SetSystemVersion(::keymaster::GetOsVersion(),
                                        ::keymaster::GetOsPatchlevel());
              return context;
          }(),
          kOperationTableSize)),
      securityLevel_(securityLevel) {}

AndroidKeyMintDevice::~AndroidKeyMintDevice() {}

ScopedAStatus AndroidKeyMintDevice::getHardwareInfo(KeyMintHardwareInfo* info) {
    info->versionNumber = 1;
    info->securityLevel = securityLevel_;
    info->keyMintName = "FakeKeyMintDevice";
    info->keyMintAuthorName = "Google";

    return ScopedAStatus::ok();
}

ScopedAStatus AndroidKeyMintDevice::addRngEntropy(const vector<uint8_t>& data) {
    if (data.size() == 0) {
        return ScopedAStatus::ok();
    }

    AddEntropyRequest request(impl_->message_version());
    request.random_data.Reinitialize(data.data(), data.size());

    AddEntropyResponse response(impl_->message_version());
    impl_->AddRngEntropy(request, &response);

    return kmError2ScopedAStatus(response.error);
}

ScopedAStatus AndroidKeyMintDevice::generateKey(const vector<KeyParameter>& keyParams,
                                                KeyCreationResult* creationResult) {

    GenerateKeyRequest request(impl_->message_version());
    request.key_description.Reinitialize(KmParamSet(keyParams));

    GenerateKeyResponse response(impl_->message_version());
    impl_->GenerateKey(request, &response);

    if (response.error != KM_ERROR_OK) {
        // Note a key difference between this current aidl and previous hal, is
        // that hal returns void where as aidl returns the error status.  If
        // aidl returns error, then aidl will not return any change you may make
        // to the out parameters.  This is quite different from hal where all
        // output variable can be modified due to hal returning void.
        //
        // So the caller need to be aware not to expect aidl functions to clear
        // the output variables for you in case of error.  If you left some
        // wrong data set in the out parameters, they will stay there.
        return kmError2ScopedAStatus(response.error);
    }

    creationResult->keyBlob = kmBlob2vector(response.key_blob);
    creationResult->keyCharacteristics =
        convertKeyCharacteristics(securityLevel_, response.unenforced, response.enforced);
    creationResult->certificateChain = convertCertificateChain(response.certificate_chain);
    return ScopedAStatus::ok();
}

ScopedAStatus AndroidKeyMintDevice::importKey(const vector<KeyParameter>& keyParams,
                                              KeyFormat keyFormat, const vector<uint8_t>& keyData,
                                              KeyCreationResult* creationResult) {

    ImportKeyRequest request(impl_->message_version());
    request.key_description.Reinitialize(KmParamSet(keyParams));
    request.key_format = legacy_enum_conversion(keyFormat);
    request.SetKeyMaterial(keyData.data(), keyData.size());

    ImportKeyResponse response(impl_->message_version());
    impl_->ImportKey(request, &response);

    if (response.error != KM_ERROR_OK) {
        return kmError2ScopedAStatus(response.error);
    }

    creationResult->keyBlob = kmBlob2vector(response.key_blob);
    creationResult->keyCharacteristics =
        convertKeyCharacteristics(securityLevel_, response.unenforced, response.enforced);
    creationResult->certificateChain = convertCertificateChain(response.certificate_chain);

    return ScopedAStatus::ok();
}

ScopedAStatus AndroidKeyMintDevice::importWrappedKey(const vector<uint8_t>& wrappedKeyData,
                                                     const vector<uint8_t>& wrappingKeyBlob,
                                                     const vector<uint8_t>& maskingKey,
                                                     const vector<KeyParameter>& unwrappingParams,
                                                     int64_t passwordSid, int64_t biometricSid,
                                                     KeyCreationResult* creationResult) {

    ImportWrappedKeyRequest request(impl_->message_version());
    request.SetWrappedMaterial(wrappedKeyData.data(), wrappedKeyData.size());
    request.SetWrappingMaterial(wrappingKeyBlob.data(), wrappingKeyBlob.size());
    request.SetMaskingKeyMaterial(maskingKey.data(), maskingKey.size());
    request.additional_params.Reinitialize(KmParamSet(unwrappingParams));
    request.password_sid = static_cast<uint64_t>(passwordSid);
    request.biometric_sid = static_cast<uint64_t>(biometricSid);

    ImportWrappedKeyResponse response(impl_->message_version());
    impl_->ImportWrappedKey(request, &response);

    if (response.error != KM_ERROR_OK) {
        return kmError2ScopedAStatus(response.error);
    }

    creationResult->keyBlob = kmBlob2vector(response.key_blob);
    creationResult->keyCharacteristics =
        convertKeyCharacteristics(securityLevel_, response.unenforced, response.enforced);
    creationResult->certificateChain = convertCertificateChain(response.certificate_chain);

    return ScopedAStatus::ok();
}

ScopedAStatus AndroidKeyMintDevice::upgradeKey(const vector<uint8_t>& keyBlobToUpgrade,
                                               const vector<KeyParameter>& upgradeParams,
                                               vector<uint8_t>* keyBlob) {

    UpgradeKeyRequest request(impl_->message_version());
    request.SetKeyMaterial(keyBlobToUpgrade.data(), keyBlobToUpgrade.size());
    request.upgrade_params.Reinitialize(KmParamSet(upgradeParams));

    UpgradeKeyResponse response(impl_->message_version());
    impl_->UpgradeKey(request, &response);

    if (response.error != KM_ERROR_OK) {
        return kmError2ScopedAStatus(response.error);
    }

    *keyBlob = kmBlob2vector(response.upgraded_key);
    return ScopedAStatus::ok();
}

ScopedAStatus AndroidKeyMintDevice::deleteKey(const vector<uint8_t>& keyBlob) {
    DeleteKeyRequest request(impl_->message_version());
    request.SetKeyMaterial(keyBlob.data(), keyBlob.size());

    DeleteKeyResponse response(impl_->message_version());
    impl_->DeleteKey(request, &response);

    return kmError2ScopedAStatus(response.error);
}

ScopedAStatus AndroidKeyMintDevice::deleteAllKeys() {
    // There's nothing to be done to delete software key blobs.
    DeleteAllKeysRequest request(impl_->message_version());
    DeleteAllKeysResponse response(impl_->message_version());
    impl_->DeleteAllKeys(request, &response);

    return kmError2ScopedAStatus(response.error);
}

ScopedAStatus AndroidKeyMintDevice::destroyAttestationIds() {
    return kmError2ScopedAStatus(KM_ERROR_UNIMPLEMENTED);
}

ScopedAStatus AndroidKeyMintDevice::begin(KeyPurpose purpose, const vector<uint8_t>& keyBlob,
                                          const vector<KeyParameter>& params,
                                          const HardwareAuthToken& authToken, BeginResult* result) {

    BeginOperationRequest request(impl_->message_version());
    request.purpose = legacy_enum_conversion(purpose);
    request.SetKeyMaterial(keyBlob.data(), keyBlob.size());
    request.additional_params.Reinitialize(KmParamSet(params));

    vector<uint8_t> vector_token = authToken2AidlVec(authToken);
    request.additional_params.push_back(
        TAG_AUTH_TOKEN, reinterpret_cast<uint8_t*>(vector_token.data()), vector_token.size());

    BeginOperationResponse response(impl_->message_version());
    impl_->BeginOperation(request, &response);

    if (response.error != KM_ERROR_OK) {
        return kmError2ScopedAStatus(response.error);
    }

    result->params = kmParamSet2Aidl(response.output_params);
    result->challenge = response.op_handle;
    result->operation =
        ndk::SharedRefBase::make<AndroidKeyMintOperation>(impl_, response.op_handle);
    return ScopedAStatus::ok();
}

IKeyMintDevice* CreateKeyMintDevice(SecurityLevel securityLevel) {

    return ::new AndroidKeyMintDevice(securityLevel);
}

}  // namespace aidl::android::hardware::security::keymint