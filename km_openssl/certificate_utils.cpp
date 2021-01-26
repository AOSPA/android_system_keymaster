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

#include <openssl/evp.h>
#include <openssl/x509v3.h>

#include <hardware/keymaster_defs.h>
#include <keymaster/android_keymaster_utils.h>
#include <keymaster/authorization_set.h>
#include <keymaster/km_openssl/asymmetric_key.h>
#include <keymaster/km_openssl/certificate_utils.h>
#include <keymaster/km_openssl/openssl_err.h>

namespace keymaster {

namespace {

constexpr const char kDefaultSubject[] = "Android Keystore Key";
constexpr int kDataEnciphermentKeyUsageBit = 3;
constexpr int kDefaultSerial = 1;
constexpr int kDigitalSignatureKeyUsageBit = 0;
constexpr int kKeyEnciphermentKeyUsageBit = 2;
constexpr int kMaxKeyUsageBit = 8;
// Per RFC 5280 4.1.2.5, an undefined expiration (not-after) field should be set to GeneralizedTime
// 999912312359559, which is 253402325999000 ms from Jan 1, 1970.
constexpr uint64_t kUndefinedExpirationDateTime = 253402325999000;

template <typename T> T&& min(T&& a, T&& b) {
    return (a < b) ? forward<T>(a) : forward<T>(b);
}

keymaster_error_t fake_sign_cert(X509* cert) {
    // Set algorithm in TBSCertificate
    X509_ALGOR_set0(cert->cert_info->signature, OBJ_nid2obj(NID_sha256WithRSAEncryption),
                    V_ASN1_NULL, nullptr);

    // Set algorithm in Certificate
    X509_ALGOR_set0(cert->sig_alg, OBJ_nid2obj(NID_sha256WithRSAEncryption), V_ASN1_NULL, nullptr);

    // Set signature to a bit string containing a single byte, value 0.
    uint8_t fake_sig = 0;
    if (!cert->signature) cert->signature = ASN1_BIT_STRING_new();
    if (!cert->signature) return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    if (!ASN1_STRING_set(cert->signature, &fake_sig, sizeof(fake_sig))) {
        return TranslateLastOpenSslError();
    }

    return KM_ERROR_OK;
}

}  // namespace

keymaster_error_t make_name_from_str(const char name[], X509_NAME_Ptr* name_out) {
    if (name_out == nullptr) return KM_ERROR_UNEXPECTED_NULL_POINTER;
    X509_NAME_Ptr x509_name(X509_NAME_new());
    if (!x509_name.get()) {
        return TranslateLastOpenSslError();
    }
    if (!X509_NAME_add_entry_by_txt(x509_name.get(),  //
                                    "CN",             //
                                    MBSTRING_ASC, reinterpret_cast<const uint8_t*>(&name[0]),
                                    -1,  // len
                                    -1,  // loc
                                    0 /* set */)) {
        return TranslateLastOpenSslError();
    }
    *name_out = move(x509_name);
    return KM_ERROR_OK;
}

keymaster_error_t make_name_from_der(const keymaster_blob_t& name, X509_NAME_Ptr* name_out) {
    if (!name_out || !name.data) return KM_ERROR_UNEXPECTED_NULL_POINTER;

    const uint8_t* p = name.data;
    X509_NAME_Ptr x509_name(d2i_X509_NAME(nullptr, &p, name.data_length));
    if (!x509_name.get()) {
        return TranslateLastOpenSslError();
    }

    *name_out = move(x509_name);
    return KM_ERROR_OK;
}

keymaster_error_t get_common_name(X509_NAME* name, UniquePtr<const char[]>* name_out) {
    if (name == nullptr || name_out == nullptr) return KM_ERROR_UNEXPECTED_NULL_POINTER;
    int len = X509_NAME_get_text_by_NID(name, NID_commonName, nullptr, 0);
    UniquePtr<char[]> name_ptr(new (std::nothrow) char[len]);
    if (!name_ptr) {
        return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    }
    X509_NAME_get_text_by_NID(name, NID_commonName, name_ptr.get(), len);
    *name_out = UniquePtr<const char[]>{name_ptr.release()};
    return KM_ERROR_OK;
}

keymaster_error_t get_certificate_params(const AuthorizationSet& caller_params,
                                         CertificateCallerParams* cert_params) {
    if (!cert_params) return KM_ERROR_UNEXPECTED_NULL_POINTER;

    cert_params->serial = kDefaultSerial;
    caller_params.GetTagValue(TAG_CERTIFICATE_SERIAL, &cert_params->serial);

    cert_params->active_date_time = 0;
    caller_params.GetTagValue(TAG_ACTIVE_DATETIME, &cert_params->active_date_time);

    cert_params->expire_date_time = kUndefinedExpirationDateTime;
    caller_params.GetTagValue(TAG_USAGE_EXPIRE_DATETIME, &cert_params->expire_date_time);

    keymaster_blob_t subject{};
    if (caller_params.GetTagValue(TAG_CERTIFICATE_SUBJECT, &subject) && subject.data_length) {
        return make_name_from_der(subject, &cert_params->subject_name);
    }
    return make_name_from_str(kDefaultSubject, &cert_params->subject_name);
}

keymaster_error_t make_key_usage_extension(bool is_signing_key, bool is_encryption_key,
                                           X509_EXTENSION_Ptr* usage_extension_out) {
    if (usage_extension_out == nullptr) return KM_ERROR_UNEXPECTED_NULL_POINTER;

    // Build BIT_STRING with correct contents.
    ASN1_BIT_STRING_Ptr key_usage(ASN1_BIT_STRING_new());
    if (!key_usage) return KM_ERROR_MEMORY_ALLOCATION_FAILED;

    for (size_t i = 0; i <= kMaxKeyUsageBit; ++i) {
        if (!ASN1_BIT_STRING_set_bit(key_usage.get(), i, 0)) {
            return TranslateLastOpenSslError();
        }
    }

    if (is_signing_key) {
        if (!ASN1_BIT_STRING_set_bit(key_usage.get(), kDigitalSignatureKeyUsageBit, 1)) {
            return TranslateLastOpenSslError();
        }
    }

    if (is_encryption_key) {
        if (!ASN1_BIT_STRING_set_bit(key_usage.get(), kKeyEnciphermentKeyUsageBit, 1) ||
            !ASN1_BIT_STRING_set_bit(key_usage.get(), kDataEnciphermentKeyUsageBit, 1)) {
            return TranslateLastOpenSslError();
        }
    }

    // Convert to octets
    int len = i2d_ASN1_BIT_STRING(key_usage.get(), nullptr);
    if (len < 0) {
        return TranslateLastOpenSslError();
    }
    UniquePtr<uint8_t[]> asn1_key_usage(new (std::nothrow) uint8_t[len]);
    if (!asn1_key_usage.get()) {
        return KM_ERROR_MEMORY_ALLOCATION_FAILED;
    }
    uint8_t* p = asn1_key_usage.get();
    len = i2d_ASN1_BIT_STRING(key_usage.get(), &p);
    if (len < 0) {
        return TranslateLastOpenSslError();
    }

    // Build OCTET_STRING
    ASN1_OCTET_STRING_Ptr key_usage_str(ASN1_OCTET_STRING_new());
    if (!key_usage_str.get() ||
        !ASN1_OCTET_STRING_set(key_usage_str.get(), asn1_key_usage.get(), len)) {
        return TranslateLastOpenSslError();
    }

    X509_EXTENSION_Ptr key_usage_extension(X509_EXTENSION_create_by_NID(nullptr,        //
                                                                        NID_key_usage,  //
                                                                        true /* critical */,
                                                                        key_usage_str.get()));
    if (!key_usage_extension.get()) {
        return TranslateLastOpenSslError();
    }

    *usage_extension_out = move(key_usage_extension);

    return KM_ERROR_OK;
}

// Creates a rump certificate structure with serial, subject and issuer names, as well as
// activation and expiry date.
// Callers should pass an empty X509_Ptr and check the return value for KM_ERROR_OK (0) before
// accessing the result.
keymaster_error_t make_cert_rump(const X509_NAME* issuer,
                                 const CertificateCallerParams& cert_params, X509_Ptr* cert_out) {
    if (!cert_out || !issuer) return KM_ERROR_UNEXPECTED_NULL_POINTER;

    // Create certificate structure.
    X509_Ptr certificate(X509_new());
    if (!certificate.get()) return TranslateLastOpenSslError();

    // Set the X509 version.
    if (!X509_set_version(certificate.get(), 2 /* version 3 */)) return TranslateLastOpenSslError();

    // Set the certificate serialNumber
    ASN1_INTEGER_Ptr serial_number(ASN1_INTEGER_new());
    if (!serial_number.get() ||  //
        !ASN1_INTEGER_set(serial_number.get(), cert_params.serial) ||
        !X509_set_serialNumber(certificate.get(),
                               serial_number.get() /* Don't release; copied */)) {
        return TranslateLastOpenSslError();
    }

    if (!X509_set_subject_name(certificate.get(),
                               const_cast<X509_NAME*>(cert_params.subject_name.get()))) {
        return TranslateLastOpenSslError();
    }

    if (!X509_set_issuer_name(certificate.get(), const_cast<X509_NAME*>(issuer))) {
        return TranslateLastOpenSslError();
    }

    // Set activation date.
    ASN1_TIME_Ptr notBefore(ASN1_TIME_new());
    time_t notBeforeTime = static_cast<time_t>(cert_params.active_date_time / 1000);
    if (!notBefore.get() || !ASN1_TIME_set(notBefore.get(), notBeforeTime) ||
        !X509_set_notBefore(certificate.get(), notBefore.get() /* Don't release; copied */)) {
        return TranslateLastOpenSslError();
    }

    // Set expiration date.
    ASN1_TIME_Ptr notAfter(ASN1_TIME_new());
    time_t notAfterTime = static_cast<time_t>(cert_params.expire_date_time / 1000);

    if (!notAfter.get() || !ASN1_TIME_set(notAfter.get(), notAfterTime) ||
        !X509_set_notAfter(certificate.get(), notAfter.get() /* Don't release; copied */)) {
        return TranslateLastOpenSslError();
    }

    *cert_out = move(certificate);
    return KM_ERROR_OK;
}

keymaster_error_t make_cert(const EVP_PKEY* evp_pkey, const X509_NAME* issuer,
                            const CertificateCallerParams& cert_params, const bool is_signing_key,
                            const bool is_encryption_key, X509_Ptr* cert_out) {

    // Make the rump certificate with serial, subject, not before and not after dates.
    X509_Ptr certificate;
    if (keymaster_error_t error = make_cert_rump(issuer, cert_params, &certificate)) {
        return error;
    }

    // Set the public key.
    if (!X509_set_pubkey(certificate.get(), (EVP_PKEY*)evp_pkey)) {
        return TranslateLastOpenSslError();
    }

    // Make and add the key usage extension.
    X509_EXTENSION_Ptr key_usage_extension;
    if (auto error =
            make_key_usage_extension(is_signing_key, is_encryption_key, &key_usage_extension)) {
        return error;
    }
    if (!X509_add_ext(certificate.get(), key_usage_extension.get() /* Don't release; copied */,
                      -1 /* insert at end */)) {
        return TranslateLastOpenSslError();
    }

    *cert_out = move(certificate);
    return KM_ERROR_OK;
}

keymaster_error_t sign_cert(X509* certificate, const EVP_PKEY* signing_key) {
    // X509_sign takes the key as non-const, but per the BoringSSL dev team, that's a legacy
    // mistake that hasn't yet been corrected.
    auto sk = const_cast<EVP_PKEY*>(signing_key);

    if (!X509_sign(certificate, sk, EVP_sha256())) {
        return TranslateLastOpenSslError();
    }
    return KM_ERROR_OK;
}

// Takes a certificate a signing certificate and the raw private signing_key. And signs
// the certificate with the latter.
keymaster_error_t sign_cert(X509* certificate, const keymaster_key_blob_t& signing_key) {
    if (!certificate) return KM_ERROR_UNEXPECTED_NULL_POINTER;

    const uint8_t* key_material = signing_key.key_material;
    EVP_PKEY_Ptr sign_key(
        d2i_AutoPrivateKey(nullptr, &key_material, signing_key.key_material_size));
    if (!sign_key) {
        return TranslateLastOpenSslError();
    }

    return sign_cert(certificate, sign_key.get());
}

CertificateChain generate_self_signed_cert(const AsymmetricKey& key, const AuthorizationSet& params,
                                           bool fake_signature, keymaster_error_t* error) {
    keymaster_error_t err;
    if (!error) error = &err;

    EVP_PKEY_Ptr pkey(EVP_PKEY_new());
    if (!key.InternalToEvp(pkey.get())) {
        *error = TranslateLastOpenSslError();
        return {};
    }

    bool is_signing_key = key.authorizations().Contains(TAG_PURPOSE, KM_PURPOSE_SIGN);
    bool is_encryption_key = key.authorizations().Contains(TAG_PURPOSE, KM_PURPOSE_DECRYPT);

    CertificateCallerParams cert_params{};
    *error = get_certificate_params(params, &cert_params);
    if (*error != KM_ERROR_OK) return {};

    X509_Ptr cert;
    *error = make_cert(pkey.get(), cert_params.subject_name.get() /* issuer */, cert_params,
                       is_signing_key, is_encryption_key, &cert);
    if (*error != KM_ERROR_OK) return {};

    if (fake_signature) {
        *error = fake_sign_cert(cert.get());
    } else {
        *error = sign_cert(cert.get(), pkey.get());
    }
    if (*error != KM_ERROR_OK) return {};

    CertificateChain result(1);
    if (!result) {
        *error = KM_ERROR_MEMORY_ALLOCATION_FAILED;
        return {};
    }

    *error = encode_certificate(cert.get(), &result.entries[0]);
    if (*error != KM_ERROR_OK) return {};

    return result;
}

keymaster_error_t encode_certificate(X509* certificate, keymaster_blob_t* blob) {
    int len = i2d_X509(certificate, nullptr /* ppout */);
    if (len < 0) return TranslateLastOpenSslError();

    blob->data = new (std::nothrow) uint8_t[len];
    if (!blob->data) return KM_ERROR_MEMORY_ALLOCATION_FAILED;

    uint8_t* p = const_cast<uint8_t*>(blob->data);
    blob->data_length = i2d_X509(certificate, &p);
    return KM_ERROR_OK;
}

}  // namespace keymaster